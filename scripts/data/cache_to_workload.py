#!/usr/bin/env python3
"""Convert cacheMon/cache_dataset or Wikimedia CDN traces into standard CSV workloads.

支持以下输入格式（自动识别，支持直接读压缩文件，也支持 http(s):// 流式截取）：
  1. cacheMon `oracleGeneral` 二进制：*.oracleGeneral.zst / .bin.zst / .bin
     （24 字节小端定长结构体 <IQIq：clock_time, obj_id, obj_size, next_access_vtime）
  2. Wikimedia 官方按天原始 TSV：cache-u*.tsv.gz / cache-t*.tsv.gz / *.tsv.zst / *.tsv
  3. 通用 CSV：*.csv / *.csv.gz / *.csv.zst（支持带表头或无表头，可指定列索引/列名）

两种转换模式（直接在下方“用户可调参数区”修改，不接受命令行传参）：
  - MODE = 'thin' : 真实时间窗切片 + 按目标平均速率（或条数/比例）独立稀释 + 子秒时间戳还原
  - MODE = 'cdf'  : 统计真实对象大小经验分布（ECDF）+ 可控泊松 / 突发到达（同 DCTCP/Homa 范式）

用法：
  修改下方参数后直接运行：
    make cache_to_workload
  或：
    python3 scripts/data/cache_to_workload.py
"""
from __future__ import annotations

import bisect
import csv
import gzip
import io
import math
import random
import shutil
import struct
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / 'scripts/bench') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts/bench'))
import lines_common  # noqa: E402

# ============================================================================
# 可选公开数据源下载地址表（CMU PDL 官方镜像）
# 全部支持直接在线流式截取：读满 SKIP_SECONDS + DURATION_S 后自动断开，无需下载整份几十 GB 文件！
# 若你已手动下载到本地 LOCAL_CACHE_DIR，脚本会自动优先读取本地文件。
# 更多目录浏览：
#   - CMU PDL 全部缓存数据集总目录：https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/
#   - Wikimedia 官方按天原始 TSV（cache-t ~100MB / cache-u ~1.5GB）：https://analytics.wikimedia.org/published/datasets/caching/
#   - cacheMon 官方公开 AWS S3 桶：s3://cache-datasets/cache_dataset_oracleGeneral/
# ============================================================================
_CMU_BASE = 'https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets'
PRESET_SOURCES: dict[str, str] = {
    # --- 强烈推荐：真实变长 CDN / Web 对象（最适合 TCP 拥塞控制重尾评测） ---
    'wiki_2019u':      f'{_CMU_BASE}/wiki/wiki_2019u.oracleGeneral.zst',              # 25 GB  | Wikimedia 2019 图片/媒体 CDN（大小跨度最丰富）
    'wiki_2019t':      f'{_CMU_BASE}/wiki/wiki_2019t.oracleGeneral.zst',              # 1.9 GB | Wikimedia 2019 文本/HTML 网页 CDN（较轻量）
    'wiki_2016u':      f'{_CMU_BASE}/wiki/wiki_2016u.oracleGeneral.zst',              # 23 GB  | Wikimedia 2016 图片/媒体 CDN
    'meta_reag':       f'{_CMU_BASE}/metaCDN/meta_reag.oracleGeneral.zst',            # 334 MB | Meta CDN（体积最小、下载/流式最快）
    'meta_rprn':       f'{_CMU_BASE}/metaCDN/meta_rprn.oracleGeneral.zst',            # 824 MB | Meta CDN
    'meta_rnha':       f'{_CMU_BASE}/metaCDN/meta_rnha.oracleGeneral.zst',            # 1.0 GB | Meta CDN
    'tencent_photo1':  f'{_CMU_BASE}/tencentPhoto/tencent_photo1.oracleGeneral.zst',  # ~35 GB | Tencent QQPhoto 图片 CDN 1
    'tencent_photo2':  f'{_CMU_BASE}/tencentPhoto/tencent_photo2.oracleGeneral.zst',  # ~32 GB | Tencent QQPhoto 图片 CDN 2
    # --- 可选：生产 KV 缓存轨迹（偏小对象 / 混合 KV，适合测 RPC / 短流） ---
    'meta_kv_202206':  f'{_CMU_BASE}/metaKV/202206_kv_traces_all.csv.oracleGeneral.zst',
    'meta_kv_202401':  f'{_CMU_BASE}/metaKV/202401_kv_traces_all_sort.csv.oracleGeneral.zst',
    'twitter_c1':      f'{_CMU_BASE}/twitter/cluster1.oracleGeneral.zst',             # Twitter Twemcache cluster 1
    'twitter_c2':      f'{_CMU_BASE}/twitter/cluster2.oracleGeneral.zst',             # Twitter Twemcache cluster 2
}

# ============================================================================
# 用户可调参数区 —— 修改这里后直接运行 `make cache_to_workload`
# ============================================================================

# 1) 输入与输出路径
# INPUT_SOURCE 可直接填：
#   - 上方 PRESET_SOURCES 里的短名称（如 'wiki_2019u' / 'wiki_2019t' / 'meta_reag' / 'tencent_photo1'）
#   - 任意 http(s):// 下载直链
#   - 本地文件路径（如 ROOT / 'datasets/traces/wiki/wiki_2019u.oracleGeneral.zst'）
INPUT_SOURCE: str | Path = 'wiki_2019u'
LOCAL_CACHE_DIR: Path = ROOT / 'datasets/traces/cache_dataset'  # 若该目录已有同名本地文件，优先读本地避免重复联网
OUTPUT_CSV: Path | None = None  # None: 自动命名为 datasets/workloads/<源名>.csv；也可指定如 ROOT / 'datasets/workloads/wiki_cdn.csv'
INPUT_FORMAT: str = 'auto'      # 'auto' | 'oracleGeneral' | 'wiki_tsv' | 'csv'
OVERWRITE: bool = True          # True: 调参反复运行时直接覆盖 OUTPUT_CSV；False: 已存在时报错保护

# 2) 核心模式与时长
MODE: str = 'thin'          # 'thin'（真实时间窗稀释，保留真实峰谷）| 'cdf'（经验分布 + 可控到达，最易控变量）
SKIP_SECONDS: float = 0.0   # 跳过 trace 开头多少秒（用于避开冷启动或切不同时间段）
DURATION_S: float = 900.0   # 目标 workload 跨度（秒）。如 900s 可完整覆盖 lumos(800s)/mahimahi(786s)

# 3) 负载强度控制（三选一优先级：SAMPLE_RATE > TARGET_MBPS > TARGET_REQUESTS）
TARGET_MBPS: float | None = 3.0     # 目标平均业务速率（Mbps）。LTE/通用建议 2~5；5G 高速建议 10~20；设为 None 则改用 TARGET_REQUESTS
TARGET_REQUESTS: int | None = None  # 目标请求总条数（仅当 TARGET_MBPS=None 且 SAMPLE_RATE=None 时生效，例如 1500）
SAMPLE_RATE: float | None = None    # 仅 MODE='thin'：显式固定抽样比例（0 < p <= 1，如 0.002 = 1/500），设置后覆盖 TARGET_MBPS

# 4) 对象大小清洗范围（可写字节整数，也可写 '1kb' / '10mb'，须在 1 .. 100,000,000 B 内）
MIN_BYTES: int | str = '1kb'        # 过滤掉 0 字节 miss 和极小元数据（例如 1kb 或 1448）
MAX_BYTES: int | str = '10mb'       # 过滤掉极端巨型对象，防止单条卡死低速蜂窝链路（硬上限 100,000,000 B）

# 5) MODE='thin' 专属：子秒时间戳还原（oracleGeneral 原始时间精度为 1 秒）
SUBSECOND_MODE: str = 'uniform'     # 'uniform'（秒内随机均匀打散）| 'even'（秒内等距）| 'burst'（带同毫秒突发）| 'raw'（原样保留）
BURST_PROB: float = 0.25            # 仅 SUBSECOND_MODE='burst'：同秒内与前一条同毫秒并发的概率

# 6) MODE='cdf' 专属：经验分布扫描与到达模型
CDF_SCAN_LIMIT: int = 1_000_000     # 最多扫描多少条有效记录来估计真实对象大小分布（<=0 表示扫描到文件尾）
CDF_RESERVOIR_SIZE: int = 200_000   # 内存中保存的经验大小样本池上限（蓄水池抽样，保证低内存且无偏）
ARRIVAL_MODEL: str = 'poisson'      # 'poisson'（标准指数到达间隔）| 'bursty'（网页式多对象簇突发 + 思考空闲）
BURST_SIZE_MIN: int = 3             # 仅 ARRIVAL_MODEL='bursty'：每次突发最少对象数
BURST_SIZE_MAX: int = 8             # 仅 ARRIVAL_MODEL='bursty'：每次突发最多对象数
BURST_INNER_GAP_MS: float = 15.0    # 仅 ARRIVAL_MODEL='bursty'：簇内相邻对象间隔（ms，0 表示同毫秒并发）

# 7) 通用 CSV 解析设置（仅 INPUT_FORMAT='csv' 时生效）
CSV_TIME_COL: int | str = 0         # 时间列（0-based 列号，或表头列名如 'timestamp'）
CSV_SIZE_COL: int | str = 2         # 大小列（0-based 列号，或表头列名如 'obj_size'）
CSV_ID_COL: int | str | None = 1    # 可选对象 ID 列（仅用于写入 comment；设为 None 不读）
CSV_TIME_UNIT: str = 's'            # 源 CSV 时间单位：'s' | 'ms' | 'us'

# 8) 输出细节与随机种子
TIME_DECIMALS: int = 1              # 输出时间戳（ms）小数位数（0=整数 ms，1=0.1 ms 精度）
ALIGN_FIRST_TO_ZERO: bool = True    # True: 首条请求平移到 time=0
INCLUDE_COMMENT: bool = True        # True: 在 comment 列写入对象 ID 或来源注释；False: 仅输出 time,size 两列
SEED: int = 42                      # 固定随机种子，保证结果完全可复现

# ============================================================================
# 实现代码
# ============================================================================

ORACLE_RECORD = struct.Struct('<IQIq')
_ORACLE_BATCH_RECORDS = 4096
_STREAM_RESERVOIR_MAX = 200_000
_MAX_WORKLOAD_BYTES = 100_000_000


@dataclass(frozen=True)
class ConvertConfig:
    input_source: str
    output_csv: Path
    input_format: str = 'auto'
    overwrite: bool = True
    mode: str = 'thin'
    skip_seconds: float = 0.0
    duration_s: float = 900.0
    target_mbps: float | None = 3.0
    target_requests: int | None = None
    sample_rate: float | None = None
    min_bytes: int = 1024
    max_bytes: int = 10 * 1024 * 1024
    subsecond_mode: str = 'uniform'
    burst_prob: float = 0.25
    cdf_scan_limit: int = 1_000_000
    cdf_reservoir_size: int = 200_000
    arrival_model: str = 'poisson'
    burst_size_min: int = 3
    burst_size_max: int = 8
    burst_inner_gap_ms: float = 15.0
    csv_time_col: int | str = 0
    csv_size_col: int | str = 2
    csv_id_col: int | str | None = 1
    csv_time_unit: str = 's'
    time_decimals: int = 1
    align_first_to_zero: bool = True
    include_comment: bool = True
    seed: int = 42


@dataclass(frozen=True)
class ConvertStats:
    output_csv: Path
    mode: str
    detected_format: str
    raw_scanned: int
    raw_valid: int
    raw_bytes: int
    raw_span_s: float
    used_sample_rate: float | None
    out_requests: int
    out_span_s: float
    out_bytes: int
    out_avg_mbps: float
    size_p50: int
    size_p95: int
    size_p99: int
    gap_p50_ms: float
    gap_p95_ms: float
    gap_max_ms: float
    idle_gaps_over_1s: int


def parse_byte_limit(value: int | str, name: str) -> int:
    if isinstance(value, bool):
        raise ValueError(f'{name} must be an integer or byte string, not bool')
    if isinstance(value, int):
        n = value
    elif isinstance(value, str):
        n = lines_common.parse_size_bytes(value)
    else:
        raise ValueError(f'invalid {name}: {value!r}')
    if n < 1 or n > _MAX_WORKLOAD_BYTES:
        raise ValueError(f'{name} must be in [1, {_MAX_WORKLOAD_BYTES}], got {n}')
    return n


def is_url(source: str) -> bool:
    lower = source.lower()
    return lower.startswith('http://') or lower.startswith('https://')


def detect_compression(source: str) -> str:
    lower = source.split('?', 1)[0].lower()
    if lower.endswith('.zst') or lower.endswith('.zstd'):
        return 'zst'
    if lower.endswith('.gz') or lower.endswith('.gzip'):
        return 'gz'
    return 'none'


def detect_format(source: str, explicit: str = 'auto') -> str:
    if explicit != 'auto':
        if explicit not in ('oracleGeneral', 'wiki_tsv', 'csv'):
            raise ValueError(f'unsupported INPUT_FORMAT: {explicit!r}')
        return explicit
    name = source.split('?', 1)[0].lower()
    for ext in ('.zst', '.zstd', '.gz', '.gzip'):
        if name.endswith(ext):
            name = name[: -len(ext)]
    if 'oraclegeneral' in name or name.endswith('.bin'):
        return 'oracleGeneral'
    if name.endswith('.tsv') or 'cache-u' in name or 'cache-t' in name:
        return 'wiki_tsv'
    if name.endswith('.csv'):
        return 'csv'
    raise ValueError(
        f'cannot auto-detect format for {source!r}; set INPUT_FORMAT to oracleGeneral, wiki_tsv, or csv'
    )


class _BinaryStream:
    """Unified streaming reader for local/URL plain, gzip, or zstd binary streams."""

    def __init__(self, source: str) -> None:
        self.source = source
        self.comp = detect_compression(source)
        self._procs: list[subprocess.Popen] = []
        self._fh = None
        self.stream = None

    def __enter__(self):
        if is_url(self.source):
            if not shutil.which('curl'):
                raise RuntimeError('curl is required for http(s):// streaming inputs')
            curl = subprocess.Popen(
                ['curl', '-fsSL', self.source],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self._procs.append(curl)
            if self.comp == 'zst':
                zstd_bin = shutil.which('zstd')
                if not zstd_bin:
                    raise RuntimeError('zstd CLI is required to stream .zst files (brew install zstd)')
                dec = subprocess.Popen(
                    [zstd_bin, '-dc'],
                    stdin=curl.stdout,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                )
                assert curl.stdout is not None
                curl.stdout.close()
                self._procs.append(dec)
                self.stream = dec.stdout
            elif self.comp == 'gz':
                self._fh = gzip.GzipFile(fileobj=curl.stdout, mode='rb')
                self.stream = self._fh
            else:
                self.stream = curl.stdout
            return self.stream

        path = Path(self.source)
        if not path.is_file():
            raise FileNotFoundError(f'input trace not found: {path}')
        if self.comp == 'zst':
            zstd_bin = shutil.which('zstd')
            if zstd_bin:
                dec = subprocess.Popen(
                    [zstd_bin, '-dc', str(path)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                )
                self._procs.append(dec)
                self.stream = dec.stdout
                return self.stream
            try:
                import zstandard as zstd  # type: ignore
            except ImportError as exc:
                raise RuntimeError(
                    'reading .zst requires either `zstd` CLI (brew install zstd) or Python `zstandard`'
                ) from exc
            self._fh = path.open('rb')
            self._zctx = zstd.ZstdDecompressor().stream_reader(self._fh)
            self.stream = self._zctx
            return self.stream
        if self.comp == 'gz':
            self._fh = gzip.open(path, 'rb')
            self.stream = self._fh
            return self.stream
        self._fh = path.open('rb')
        self.stream = self._fh
        return self.stream

    def __exit__(self, exc_type, exc, tb) -> None:
        if hasattr(self, '_zctx') and self._zctx is not None:
            try:
                self._zctx.close()
            except Exception:
                pass
        if self.stream is not None:
            try:
                self.stream.close()
            except Exception:
                pass
        if self._fh is not None:
            try:
                self._fh.close()
            except Exception:
                pass
        for proc in reversed(self._procs):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def _iter_oracle_general(source: str) -> Iterator[tuple[float, int, str]]:
    """Yield (time_s, obj_size, obj_id_str) from 24-byte packed <IQIq oracleGeneral streams."""
    rec_size = ORACLE_RECORD.size
    batch_bytes = rec_size * _ORACLE_BATCH_RECORDS
    unpack_iter = ORACLE_RECORD.iter_unpack
    with _BinaryStream(source) as stream:
        rem = b''
        while True:
            chunk = stream.read(batch_bytes)
            if not chunk:
                break
            data = rem + chunk
            full = (len(data) // rec_size) * rec_size
            rem = data[full:]
            if full:
                for clock_time, obj_id, obj_size, _next_vtime in unpack_iter(memoryview(data)[:full]):
                    yield float(clock_time), int(obj_size), str(obj_id)


def _iter_wiki_tsv(source: str) -> Iterator[tuple[float, int, str]]:
    """Yield (time_s, response_size, hashed_id) from Wikimedia caching TSV files (cache-u / cache-t)."""
    with _BinaryStream(source) as raw_stream:
        text_stream = io.TextIOWrapper(raw_stream, encoding='utf-8', errors='replace', newline='')
        t_idx = 0
        id_idx = 1
        sz_idx: int | None = None
        for line_no, raw in enumerate(text_stream, 1):
            s = raw.strip()
            if not s or s.startswith('#'):
                continue
            parts = s.split('\t') if '\t' in s else s.split()
            if line_no == 1 and not parts[0].replace('.', '', 1).isdigit():
                lower = [c.strip().lower() for c in parts]
                t_idx = next((i for i, c in enumerate(lower) if 'unix' in c or 'time' in c), 0)
                id_idx = next((i for i, c in enumerate(lower) if 'hash' in c or 'key' in c or 'id' in c), 1)
                sz_idx = next((i for i, c in enumerate(lower) if 'size' in c), None)
                if sz_idx is None:
                    raise ValueError(f'{source}: cannot find response_size column in TSV header {parts!r}')
                continue
            if sz_idx is None:
                # Wikimedia cache-u: relative_unix, hashed_path_query, image_type, response_size, time_firstbyte
                # Wikimedia cache-t: relative_unix, hashed_host_path_query, response_size, time_firstbyte
                if len(parts) >= 5 and not parts[2].isdigit() and parts[3].isdigit():
                    sz_idx = 3
                elif len(parts) >= 3 and parts[2].isdigit():
                    sz_idx = 2
                else:
                    raise ValueError(f'{source}:{line_no}: cannot infer size column from {parts!r}')
            if max(t_idx, sz_idx) >= len(parts):
                continue
            try:
                t_s = float(parts[t_idx])
                sz = int(float(parts[sz_idx]))
            except ValueError:
                continue
            obj_id = parts[id_idx] if id_idx < len(parts) else ''
            yield t_s, sz, obj_id


def _resolve_csv_col(col: int | str | None, header: list[str] | None, label: str) -> int | None:
    if col is None:
        return None
    if isinstance(col, int):
        if col < 0:
            raise ValueError(f'{label} index must be >= 0, got {col}')
        return col
    if header is None:
        raise ValueError(f'{label}={col!r} requires a CSV header row')
    lower = [c.strip().lower() for c in header]
    target = col.strip().lower()
    if target not in lower:
        raise ValueError(f'{label}={col!r} not found in CSV header {header!r}')
    return lower.index(target)


def _iter_general_csv(source: str, cfg: ConvertConfig) -> Iterator[tuple[float, int, str]]:
    """Yield (time_s, size_bytes, id_str) from general headered or headerless CSV files."""
    unit_scale = {'s': 1.0, 'ms': 1e-3, 'us': 1e-6}.get(cfg.csv_time_unit.lower())
    if unit_scale is None:
        raise ValueError(f'invalid CSV_TIME_UNIT: {cfg.csv_time_unit!r} (expected s, ms, or us)')

    with _BinaryStream(source) as raw_stream:
        text_stream = io.TextIOWrapper(raw_stream, encoding='utf-8-sig', errors='replace', newline='')
        reader = csv.reader(text_stream)
        t_idx: int | None = None
        sz_idx: int | None = None
        id_idx: int | None = None
        first_row = True
        for row in reader:
            if not row or not row[0].strip() or row[0].lstrip().startswith('#'):
                continue
            if first_row:
                first_row = False
                # Detect whether row 1 is a header (non-numeric where time/size columns live or string col names)
                is_header = isinstance(cfg.csv_time_col, str) or isinstance(cfg.csv_size_col, str)
                if not is_header:
                    try:
                        float(row[int(cfg.csv_time_col)].strip())
                        float(row[int(cfg.csv_size_col)].strip())
                    except (ValueError, IndexError):
                        is_header = True
                if is_header:
                    t_idx = _resolve_csv_col(cfg.csv_time_col, row, 'CSV_TIME_COL')
                    sz_idx = _resolve_csv_col(cfg.csv_size_col, row, 'CSV_SIZE_COL')
                    id_idx = _resolve_csv_col(cfg.csv_id_col, row, 'CSV_ID_COL')
                    continue
                t_idx = int(cfg.csv_time_col)
                sz_idx = int(cfg.csv_size_col)
                id_idx = int(cfg.csv_id_col) if cfg.csv_id_col is not None else None
            assert t_idx is not None and sz_idx is not None
            if max(t_idx, sz_idx) >= len(row):
                continue
            try:
                t_s = float(row[t_idx].strip()) * unit_scale
                sz = int(round(float(row[sz_idx].strip())))
            except ValueError:
                continue
            obj_id = row[id_idx].strip() if (id_idx is not None and id_idx < len(row)) else ''
            yield t_s, sz, obj_id


def iter_records(source: str, fmt: str, cfg: ConvertConfig) -> Iterator[tuple[float, int, str]]:
    if fmt == 'oracleGeneral':
        return _iter_oracle_general(source)
    if fmt == 'wiki_tsv':
        return _iter_wiki_tsv(source)
    if fmt == 'csv':
        return _iter_general_csv(source, cfg)
    raise ValueError(f'unsupported format: {fmt!r}')


def _percentile(sorted_vals: list[float] | list[int], p: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = int(math.ceil(p / 100.0 * len(sorted_vals)))
    idx = min(max(idx, 1), len(sorted_vals)) - 1
    return float(sorted_vals[idx])


def _apply_subsecond_timestamps(
    kept: list[tuple[float, int, str]],
    cfg: ConvertConfig,
    rng: random.Random,
) -> list[tuple[float, int, str]]:
    if cfg.subsecond_mode == 'raw':
        out = [(t_s * 1000.0, sz, oid) for t_s, sz, oid in kept]
    else:
        by_sec: dict[int, list[tuple[int, str]]] = defaultdict(list)
        for t_s, sz, oid in kept:
            by_sec[int(math.floor(t_s))].append((sz, oid))
        out = []
        for sec in sorted(by_sec):
            bucket = by_sec[sec]
            n = len(bucket)
            if cfg.subsecond_mode == 'even':
                offsets = [(i * 1000.0) / n for i in range(n)]
            elif cfg.subsecond_mode == 'uniform':
                offsets = sorted(rng.random() * 1000.0 for _ in range(n))
            elif cfg.subsecond_mode == 'burst':
                # Partition bucket into micro-bursts that share an identical millisecond timestamp
                raw_offsets = sorted(rng.random() * 1000.0 for _ in range(n))
                offsets = []
                for idx, off in enumerate(raw_offsets):
                    if idx > 0 and rng.random() < cfg.burst_prob:
                        offsets.append(offsets[-1])
                    else:
                        offsets.append(off)
            else:
                raise ValueError(f'invalid SUBSECOND_MODE: {cfg.subsecond_mode!r}')
            base_ms = sec * 1000.0
            for off, (sz, oid) in zip(offsets, bucket):
                out.append((base_ms + off, sz, oid))

    out.sort(key=lambda item: item[0])
    if not out:
        return out
    origin = out[0][0] if cfg.align_first_to_zero else 0.0
    factor = 10 ** cfg.time_decimals
    normalized: list[tuple[float, int, str]] = []
    prev_ms = 0.0
    for t_ms, sz, oid in out:
        rel = max(0.0, t_ms - origin)
        rel = round(rel * factor) / factor if cfg.time_decimals > 0 else float(int(round(rel)))
        if rel < prev_ms:
            rel = prev_ms
        normalized.append((rel, sz, oid))
        prev_ms = rel
    return normalized


def _convert_thin(cfg: ConvertConfig, fmt: str) -> tuple[list[tuple[float, int, str]], dict]:
    end_rel = cfg.skip_seconds + cfg.duration_s
    rng = random.Random(cfg.seed)

    def window_iter() -> Iterator[tuple[float, int, str, int]]:
        """Yield (window_rel_s, size, obj_id, raw_scanned_so_far) inside [skip, skip + duration)."""
        t0: float | None = None
        scanned = 0
        for raw_t, sz, oid in iter_records(cfg.input_source, fmt, cfg):
            scanned += 1
            if t0 is None:
                t0 = raw_t
            rel = raw_t - t0
            if rel < 0:
                rel = 0.0
            if rel < cfg.skip_seconds:
                continue
            if rel >= end_rel:
                break
            yield rel - cfg.skip_seconds, sz, oid, scanned

    # Fast single-pass path when fixed SAMPLE_RATE or TARGET_REQUESTS is specified
    if cfg.sample_rate is not None:
        if not (0.0 < cfg.sample_rate <= 1.0):
            raise ValueError(f'SAMPLE_RATE must be in (0, 1], got {cfg.sample_rate}')
        raw_scanned = raw_valid = raw_bytes = 0
        max_rel = 0.0
        kept: list[tuple[float, int, str]] = []
        for rel_s, sz, oid, scanned in window_iter():
            raw_scanned = scanned
            max_rel = max(max_rel, rel_s)
            if not (cfg.min_bytes <= sz <= cfg.max_bytes):
                continue
            raw_valid += 1
            raw_bytes += sz
            if cfg.sample_rate >= 1.0 or rng.random() < cfg.sample_rate:
                kept.append((rel_s, sz, oid))
        if raw_valid == 0:
            raise ValueError('no valid records matched size/window filters')
        if not kept:
            raise ValueError(f'SAMPLE_RATE={cfg.sample_rate} kept 0 records out of {raw_valid}; increase SAMPLE_RATE')
        if max_rel <= 0.0 and cfg.subsecond_mode == 'raw' and len(kept) > 1:
            raise ValueError('trace timestamps never advance (all identical); use MODE="cdf" instead')
        meta = {
            'raw_scanned': raw_scanned,
            'raw_valid': raw_valid,
            'raw_bytes': raw_bytes,
            'raw_span_s': max(max_rel, 1.0),
            'used_sample_rate': cfg.sample_rate,
        }
        return _apply_subsecond_timestamps(kept, cfg, rng), meta

    if cfg.target_mbps is None:
        if cfg.target_requests is None or cfg.target_requests < 1:
            raise ValueError('set TARGET_MBPS, TARGET_REQUESTS, or SAMPLE_RATE')
        raw_scanned = raw_valid = raw_bytes = 0
        max_rel = 0.0
        reservoir: list[tuple[float, int, str]] = []
        for rel_s, sz, oid, scanned in window_iter():
            raw_scanned = scanned
            max_rel = max(max_rel, rel_s)
            if not (cfg.min_bytes <= sz <= cfg.max_bytes):
                continue
            raw_valid += 1
            raw_bytes += sz
            if len(reservoir) < cfg.target_requests:
                reservoir.append((rel_s, sz, oid))
            else:
                j = rng.randrange(raw_valid)
                if j < cfg.target_requests:
                    reservoir[j] = (rel_s, sz, oid)
        if not reservoir:
            raise ValueError('no valid records matched size/window filters')
        if max_rel <= 0.0 and cfg.duration_s > 1.0:
            raise ValueError(
                'trace timestamps do not advance across the window (all timestamps identical); '
                'switch to MODE="cdf" to synthesize arrivals from its real object-size distribution'
            )
        used_p = min(1.0, len(reservoir) / max(raw_valid, 1))
        meta = {
            'raw_scanned': raw_scanned,
            'raw_valid': raw_valid,
            'raw_bytes': raw_bytes,
            'raw_span_s': max(max_rel, 1.0),
            'used_sample_rate': used_p,
        }
        return _apply_subsecond_timestamps(reservoir, cfg, rng), meta

    # TARGET_MBPS mode
    if cfg.target_mbps <= 0:
        raise ValueError(f'TARGET_MBPS must be > 0, got {cfg.target_mbps}')

    if is_url(cfg.input_source):
        # Single-pass streaming URL: collect bounded reservoir + exact byte/count totals
        raw_scanned = raw_valid = raw_bytes = 0
        max_rel = 0.0
        reservoir = []
        for rel_s, sz, oid, scanned in window_iter():
            raw_scanned = scanned
            max_rel = max(max_rel, rel_s)
            if not (cfg.min_bytes <= sz <= cfg.max_bytes):
                continue
            raw_valid += 1
            raw_bytes += sz
            if len(reservoir) < _STREAM_RESERVOIR_MAX:
                reservoir.append((rel_s, sz, oid))
            else:
                j = rng.randrange(raw_valid)
                if j < _STREAM_RESERVOIR_MAX:
                    reservoir[j] = (rel_s, sz, oid)
        if raw_valid == 0:
            raise ValueError('no valid records matched size/window filters')
        actual_span = max(max_rel, 1.0)
        target_bytes = cfg.target_mbps * 1e6 * actual_span / 8.0
        overall_p = min(1.0, target_bytes / max(raw_bytes, 1))
        res_p = len(reservoir) / raw_valid
        keep_prob = min(1.0, overall_p / max(res_p, 1e-12))
        kept = [rec for rec in reservoir if keep_prob >= 1.0 or rng.random() < keep_prob]
        if not kept:
            kept = [reservoir[0]]
    else:
        # Local file: exact 2-pass streaming (O(1) RAM in pass 1)
        raw_scanned = raw_valid = raw_bytes = 0
        max_rel = 0.0
        first_valid: tuple[float, int, str] | None = None
        for rel_s, sz, oid, scanned in window_iter():
            raw_scanned = scanned
            max_rel = max(max_rel, rel_s)
            if not (cfg.min_bytes <= sz <= cfg.max_bytes):
                continue
            raw_valid += 1
            raw_bytes += sz
            if first_valid is None:
                first_valid = (rel_s, sz, oid)
        if raw_valid == 0:
            raise ValueError('no valid records matched size/window filters')
        if max_rel <= 0.0 and cfg.duration_s > 1.0:
            raise ValueError(
                'trace timestamps do not advance across the window (all timestamps identical); '
                'switch to MODE="cdf" to synthesize arrivals from its real object-size distribution'
            )
        actual_span = max(max_rel, 1.0)
        target_bytes = cfg.target_mbps * 1e6 * actual_span / 8.0
        overall_p = min(1.0, target_bytes / max(raw_bytes, 1))

        kept = []
        for rel_s, sz, oid, _scanned in window_iter():
            if not (cfg.min_bytes <= sz <= cfg.max_bytes):
                continue
            if overall_p >= 1.0 or rng.random() < overall_p:
                kept.append((rel_s, sz, oid))
        if not kept and first_valid is not None:
            kept = [first_valid]

    meta = {
        'raw_scanned': raw_scanned,
        'raw_valid': raw_valid,
        'raw_bytes': raw_bytes,
        'raw_span_s': max(max_rel, 1.0),
        'used_sample_rate': overall_p,
    }
    return _apply_subsecond_timestamps(kept, cfg, rng), meta


def _convert_cdf(cfg: ConvertConfig, fmt: str) -> tuple[list[tuple[float, int, str]], dict]:
    """Build empirical object-size distribution from trace and generate controlled arrivals."""
    rng = random.Random(cfg.seed)
    pool: list[int] = []
    raw_scanned = raw_valid = raw_bytes = 0
    t0: float | None = None
    max_rel = 0.0

    for raw_t, sz, _oid in iter_records(cfg.input_source, fmt, cfg):
        raw_scanned += 1
        if t0 is None:
            t0 = raw_t
        rel = max(0.0, raw_t - t0)
        if rel < cfg.skip_seconds:
            continue
        max_rel = max(max_rel, rel - cfg.skip_seconds)
        if not (cfg.min_bytes <= sz <= cfg.max_bytes):
            continue
        raw_valid += 1
        raw_bytes += sz
        if len(pool) < cfg.cdf_reservoir_size:
            pool.append(sz)
        else:
            j = rng.randrange(raw_valid)
            if j < cfg.cdf_reservoir_size:
                pool[j] = sz
        if cfg.cdf_scan_limit > 0 and raw_valid >= cfg.cdf_scan_limit:
            break

    if not pool:
        raise ValueError('no valid records found to build empirical size distribution')

    mean_bytes = sum(pool) / len(pool)
    if cfg.target_mbps is not None:
        if cfg.target_mbps <= 0:
            raise ValueError(f'TARGET_MBPS must be > 0, got {cfg.target_mbps}')
        req_rate = (cfg.target_mbps * 1e6 / 8.0) / mean_bytes
        fixed_count: int | None = None
    elif cfg.target_requests is not None and cfg.target_requests >= 1:
        req_rate = cfg.target_requests / max(cfg.duration_s, 1e-6)
        fixed_count = cfg.target_requests
    else:
        raise ValueError('MODE="cdf" requires TARGET_MBPS or TARGET_REQUESTS')

    generated: list[tuple[float, int, str]] = []
    factor = 10 ** cfg.time_decimals

    def fmt_ms(t_ms: float) -> float:
        return round(t_ms * factor) / factor if cfg.time_decimals > 0 else float(int(round(t_ms)))

    if cfg.arrival_model == 'poisson':
        t_s = 0.0
        idx = 0
        while True:
            if fixed_count is not None and idx >= fixed_count:
                break
            if fixed_count is None and idx > 0 and t_s > cfg.duration_s:
                break
            sz = pool[rng.randrange(len(pool))]
            generated.append((fmt_ms(t_s * 1000.0), sz, f'cdf_poisson#{idx + 1}'))
            idx += 1
            t_s += rng.expovariate(req_rate)
    elif cfg.arrival_model == 'bursty':
        if cfg.burst_size_min < 1 or cfg.burst_size_max < cfg.burst_size_min:
            raise ValueError('BURST_SIZE_MIN/MAX must satisfy 1 <= min <= max')
        mean_burst = 0.5 * (cfg.burst_size_min + cfg.burst_size_max)
        burst_rate = req_rate / mean_burst
        t_ms = 0.0
        burst_id = 0
        while True:
            if fixed_count is not None and len(generated) >= fixed_count:
                break
            if fixed_count is None and burst_id > 0 and t_ms > cfg.duration_s * 1000.0:
                break
            burst_id += 1
            k = rng.randint(cfg.burst_size_min, cfg.burst_size_max)
            if fixed_count is not None:
                k = min(k, fixed_count - len(generated))
            for j in range(k):
                at_ms = t_ms + j * cfg.burst_inner_gap_ms
                if fixed_count is None and burst_id > 1 and at_ms > cfg.duration_s * 1000.0:
                    break
                sz = pool[rng.randrange(len(pool))]
                generated.append((fmt_ms(at_ms), sz, f'burst={burst_id}#{j + 1}'))
            t_ms += rng.expovariate(burst_rate) * 1000.0
    else:
        raise ValueError(f'invalid ARRIVAL_MODEL: {cfg.arrival_model!r} (expected poisson or bursty)')

    # Enforce strict non-decreasing timestamps after rounding
    prev = 0.0
    out: list[tuple[float, int, str]] = []
    for t_ms, sz, note in generated:
        t_ms = max(prev, t_ms)
        out.append((t_ms, sz, note))
        prev = t_ms

    meta = {
        'raw_scanned': raw_scanned,
        'raw_valid': raw_valid,
        'raw_bytes': raw_bytes,
        'raw_span_s': max_rel,
        'used_sample_rate': None,
    }
    return out, meta


def convert_to_workload(cfg: ConvertConfig) -> ConvertStats:
    if cfg.duration_s <= 0 or not math.isfinite(cfg.duration_s):
        raise ValueError(f'DURATION_S must be positive finite, got {cfg.duration_s}')
    if cfg.skip_seconds < 0 or not math.isfinite(cfg.skip_seconds):
        raise ValueError(f'SKIP_SECONDS must be >= 0, got {cfg.skip_seconds}')
    if cfg.min_bytes > cfg.max_bytes:
        raise ValueError(f'MIN_BYTES ({cfg.min_bytes}) must be <= MAX_BYTES ({cfg.max_bytes})')
    if cfg.output_csv.exists() and not cfg.overwrite:
        raise FileExistsError(f'output already exists (set OVERWRITE=True in script): {cfg.output_csv}')

    fmt = detect_format(cfg.input_source, cfg.input_format)
    if cfg.mode == 'thin':
        rows, meta = _convert_thin(cfg, fmt)
    elif cfg.mode == 'cdf':
        rows, meta = _convert_cdf(cfg, fmt)
    else:
        raise ValueError(f'invalid MODE: {cfg.mode!r} (expected thin or cdf)')

    items = [(t_ms, sz) for t_ms, sz, _ in rows]
    comments = (
        {
            idx: (f'obj={note}' if cfg.mode == 'thin' and note else note)
            for idx, (_, _, note) in enumerate(rows)
            if note
        }
        if cfg.include_comment
        else None
    )
    lines_common.write_workload_csv(
        cfg.output_csv,
        items,
        comments=comments,
        include_comment_col=bool(comments),
    )

    # Strict round-trip validation via the canonical benchmark parser
    verified = lines_common.parse_workload(cfg.output_csv)
    if len(verified) != len(items):
        raise RuntimeError(f'round-trip count mismatch: wrote {len(items)}, parsed {len(verified)}')

    sizes = sorted(sz for _, sz in verified)
    gaps = sorted(verified[i][0] - verified[i - 1][0] for i in range(1, len(verified)))
    span_s = verified[-1][0] / 1000.0
    total_bytes = sum(sizes)
    avg_mbps = (total_bytes * 8.0 / max(span_s, 1e-6)) / 1e6
    idle_1s = len(gaps) - bisect.bisect_right(gaps, 1000.0)

    return ConvertStats(
        output_csv=cfg.output_csv,
        mode=cfg.mode,
        detected_format=fmt,
        raw_scanned=meta['raw_scanned'],
        raw_valid=meta['raw_valid'],
        raw_bytes=meta['raw_bytes'],
        raw_span_s=meta['raw_span_s'],
        used_sample_rate=meta['used_sample_rate'],
        out_requests=len(verified),
        out_span_s=span_s,
        out_bytes=total_bytes,
        out_avg_mbps=avg_mbps,
        size_p50=int(_percentile(sizes, 50.0)),
        size_p95=int(_percentile(sizes, 95.0)),
        size_p99=int(_percentile(sizes, 99.0)),
        gap_p50_ms=_percentile(gaps, 50.0),
        gap_p95_ms=_percentile(gaps, 95.0),
        gap_max_ms=gaps[-1] if gaps else 0.0,
        idle_gaps_over_1s=idle_1s,
    )


def resolve_input_source(raw: str | Path, *, cache_dir: Path | None = None) -> tuple[str, str]:
    """Resolve preset key, URL, or local path into (actual_source_str, default_stem)."""
    key = str(raw).strip()
    if key in PRESET_SOURCES:
        url = PRESET_SOURCES[key]
        filename = url.rsplit('/', 1)[-1]
        if cache_dir is not None:
            local_hit = cache_dir / filename
            if local_hit.is_file():
                return str(local_hit), key
        return url, key
    name = key.split('?', 1)[0].rsplit('/', 1)[-1]
    stem = name
    for suffix in ('.zst', '.zstd', '.gz', '.gzip', '.oracleGeneral', '.bin', '.tsv', '.csv'):
        if stem.lower().endswith(suffix.lower()):
            stem = stem[: -len(suffix)]
    stem = ''.join(ch if (ch.isalnum() or ch in '_-') else '_' for ch in stem).strip('_-') or 'workload'
    return key, stem


def build_config() -> ConvertConfig:
    """Build conversion config purely from top-of-file tunable constants."""
    actual_source, default_stem = resolve_input_source(INPUT_SOURCE, cache_dir=LOCAL_CACHE_DIR)
    out_csv = Path(OUTPUT_CSV) if OUTPUT_CSV is not None else (ROOT / 'datasets/workloads' / f'{default_stem}.csv')
    return ConvertConfig(
        input_source=actual_source,
        output_csv=out_csv,
        input_format=INPUT_FORMAT,
        overwrite=OVERWRITE,
        mode=MODE,
        skip_seconds=SKIP_SECONDS,
        duration_s=DURATION_S,
        target_mbps=TARGET_MBPS,
        target_requests=TARGET_REQUESTS,
        sample_rate=SAMPLE_RATE,
        min_bytes=parse_byte_limit(MIN_BYTES, 'MIN_BYTES'),
        max_bytes=parse_byte_limit(MAX_BYTES, 'MAX_BYTES'),
        subsecond_mode=SUBSECOND_MODE,
        burst_prob=BURST_PROB,
        cdf_scan_limit=CDF_SCAN_LIMIT,
        cdf_reservoir_size=CDF_RESERVOIR_SIZE,
        arrival_model=ARRIVAL_MODEL,
        burst_size_min=BURST_SIZE_MIN,
        burst_size_max=BURST_SIZE_MAX,
        burst_inner_gap_ms=BURST_INNER_GAP_MS,
        csv_time_col=CSV_TIME_COL,
        csv_size_col=CSV_SIZE_COL,
        csv_id_col=CSV_ID_COL,
        csv_time_unit=CSV_TIME_UNIT,
        time_decimals=TIME_DECIMALS,
        align_first_to_zero=ALIGN_FIRST_TO_ZERO,
        include_comment=INCLUDE_COMMENT,
        seed=SEED,
    )


def print_summary(stats: ConvertStats) -> None:
    raw_mbps = (stats.raw_bytes * 8.0 / max(stats.raw_span_s, 1e-6)) / 1e6 if stats.raw_span_s > 0 else 0.0
    print(f'已生成标准 CSV 负载：{stats.output_csv}')
    print(f'  源格式 / 模式  ：{stats.detected_format} / {stats.mode}')
    if stats.used_sample_rate is not None:
        print(
            f'  原始窗口统计   ：扫描 {stats.raw_scanned:,} 条，有效 {stats.raw_valid:,} 条，'
            f'原始总量 {stats.raw_bytes / 1e6:.2f} MB（~{raw_mbps:.1f} Mbps），稀释比例 p={stats.used_sample_rate:.6g}'
        )
    else:
        print(
            f'  经验分布来源   ：扫描 {stats.raw_scanned:,} 条，有效 {stats.raw_valid:,} 条'
            f'（均值 {stats.raw_bytes / max(stats.raw_valid, 1) / 1024:.1f} KiB）'
        )
    print(
        f'  输出负载统计   ：{stats.out_requests:,} 条请求  |  跨度 {stats.out_span_s:.2f} s  |  '
        f'总数据 {stats.out_bytes / 1e6:.2f} MB  |  平均速率 {stats.out_avg_mbps:.2f} Mbps'
    )
    print(
        f'  对象大小分位   ：P50={stats.size_p50 / 1024:.1f} KiB  |  '
        f'P95={stats.size_p95 / 1024:.1f} KiB  |  P99={stats.size_p99 / 1024:.1f} KiB'
    )
    print(
        f'  到达间隔分位   ：P50={stats.gap_p50_ms:.1f} ms  |  P95={stats.gap_p95_ms:.1f} ms  |  '
        f'Max={stats.gap_max_ms:.1f} ms  |  >1s 空窗 {stats.idle_gaps_over_1s} 次'
    )
    if stats.out_span_s > 1200.0:
        print(
            '  [提示] 负载跨度超过 1200 s，运行 run_trace_bench.py 时请确认 '
            'WORKLOAD_MAX_SIMULATION_S（当前默认 1800 s）留有充足排空余量。'
        )


def main() -> int:
    try:
        cfg = build_config()
        stats = convert_to_workload(cfg)
    except (ValueError, OSError, RuntimeError) as exc:
        print(f'错误：{exc}', file=sys.stderr)
        return 1
    print_summary(stats)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
