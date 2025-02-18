import pybuda._C
from _typeshed import Incomplete
from typing import ClassVar, Dict, List, Tuple, overload

class BackendApi:
    def __init__(self, arg0: str, arg1: BackendConfig) -> None: ...
    def finish(self) -> BackendStatusCode: ...
    def get_queue_descriptor(self, arg0: str) -> DramIODesc: ...
    @overload
    def initialize(self) -> BackendStatusCode: ...
    @overload
    def initialize(self, arg0: BackendCompileResult) -> BackendStatusCode: ...
    def run_program(self, arg0: str, arg1: Dict[str, str]) -> BackendStatusCode: ...
    def wait_for_idle(self) -> BackendStatusCode: ...

class BackendCompileFailure:
    __members__: ClassVar[dict] = ...  # read-only
    BlobGen: ClassVar[BackendCompileFailure] = ...
    BriscCompile: ClassVar[BackendCompileFailure] = ...
    EriscCompile: ClassVar[BackendCompileFailure] = ...
    Invalid: ClassVar[BackendCompileFailure] = ...
    L1Size: ClassVar[BackendCompileFailure] = ...
    Net2Pipe: ClassVar[BackendCompileFailure] = ...
    NriscCompile: ClassVar[BackendCompileFailure] = ...
    OverlaySize: ClassVar[BackendCompileFailure] = ...
    PipeGen: ClassVar[BackendCompileFailure] = ...
    __entries: ClassVar[dict] = ...
    def __init__(self, value: int) -> None: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class BackendCompileResult:
    device_id: int
    extra_size_bytes: int
    failure_message: str
    failure_target: str
    failure_type: BackendCompileFailure
    logical_core_x: int
    logical_core_y: int
    success: bool
    temporal_epoch_id: int
    def __init__(self) -> None: ...

class BackendConfig:
    def __init__(self, arg0, arg1, arg2, arg3: int, arg4: str, arg5: str, arg6: str) -> None: ...
    def set_golden_ignore_df_precision(self, arg0: bool) -> None: ...
    def set_performance_trace_args(self, arg0: str) -> None: ...
    def set_runtime_args(self, arg0: str) -> None: ...

class BackendDevice:
    __members__: ClassVar[dict] = ...  # read-only
    Grayskull: ClassVar[BackendDevice] = ...
    Invalid: ClassVar[BackendDevice] = ...
    Wormhole: ClassVar[BackendDevice] = ...
    Wormhole_B0: ClassVar[BackendDevice] = ...
    __entries: ClassVar[dict] = ...
    def __init__(self, value: int) -> None: ...
    def from_json(self) -> BackendDevice: ...
    @classmethod
    def from_string(cls, arg0: str) -> BackendDevice: ...
    def to_json(self) -> str: ...
    def to_string(self) -> str: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class BackendDeviceDesc:
    def __init__(self) -> None: ...
    @property
    def arch(self) -> BackendDevice: ...
    @property
    def harvesting_mask(self) -> int: ...
    @property
    def mmio(self) -> bool: ...
    @property
    def soc_desc_yaml(self) -> str: ...

class BackendStatusCode:
    __members__: ClassVar[dict] = ...  # read-only
    RuntimeError: ClassVar[BackendStatusCode] = ...
    Success: ClassVar[BackendStatusCode] = ...
    TimeoutError: ClassVar[BackendStatusCode] = ...
    __entries: ClassVar[dict] = ...
    def __init__(self, value: int) -> None: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class BackendType:
    __members__: ClassVar[dict] = ...  # read-only
    Golden: ClassVar[BackendType] = ...
    Model: ClassVar[BackendType] = ...
    NoBackend: ClassVar[BackendType] = ...
    Silicon: ClassVar[BackendType] = ...
    __entries: ClassVar[dict] = ...
    def __init__(self, value: int) -> None: ...
    def from_json(self) -> BackendType: ...
    @classmethod
    def from_string(cls, arg0: str) -> BackendType: ...
    def to_json(self) -> str: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class DeviceConfig:
    @overload
    def __init__(self, arg0: str, arg1: str, arg2: str, arg3: str, arg4: str, arg5: bool, arg6: List[int]) -> None: ...
    @overload
    def __init__(self, arg0: str, arg1: str, arg2: str, arg3: str, arg4: str, arg5: bool, arg6: List[Tuple[int, int, int, int]]) -> None: ...
    def get_dram_backend_reserved_max(self) -> int: ...
    def get_ethernet_connections(self) -> Dict[int, Dict[int, Tuple[int, int]]]: ...
    def get_harvested_cfg(self) -> List[int]: ...
    def get_host_memory_channel_size(self, arg0: int) -> int: ...
    def get_host_memory_channel_start_address(self) -> int: ...
    def get_host_memory_num_channels(self) -> int: ...
    @property
    def arch(self) -> BackendDevice: ...
    @property
    def arch_name(self) -> str: ...
    @property
    def backend_type(self) -> str: ...
    @property
    def chip_ids(self) -> List[int]: ...
    @property
    def cluster_config_yaml(self) -> str: ...
    @property
    def device_yaml(self) -> str: ...
    @property
    def grid_size(self) -> DeviceGrid: ...

class DeviceGrid:
    def __init__(self, arg0: Tuple[int, int]) -> None: ...
    @property
    def c(self) -> int: ...
    @property
    def r(self) -> int: ...

class DeviceMode:
    __members__: ClassVar[dict] = ...  # read-only
    CompileAndRun: ClassVar[DeviceMode] = ...
    CompileOnly: ClassVar[DeviceMode] = ...
    RunOnly: ClassVar[DeviceMode] = ...
    __entries: ClassVar[dict] = ...
    def __init__(self, value: int) -> None: ...
    def from_json(self) -> DeviceMode: ...
    def to_json(self) -> str: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class DramIODesc:
    bufq_grid_dim_c: int
    bufq_grid_dim_r: int
    hstack_factor: int
    input_count: int
    mblock_m: int
    mblock_n: int
    netlist_path: str
    s_descriptor: StrideDescriptor
    stack_row_major: bool
    t: int
    tile_height: int
    tile_width: int
    ublock_ct: int
    ublock_rt: int
    vstack_factor: int
    def __init__(self, *args, **kwargs) -> None: ...
    @property
    def data_format(self) -> pybuda._C.DataFormat: ...
    @property
    def name(self) -> str: ...

class IOLayout:
    __members__: ClassVar[dict] = ...  # read-only
    Flat: ClassVar[IOLayout] = ...
    Invalid: ClassVar[IOLayout] = ...
    Tilized: ClassVar[IOLayout] = ...
    __entries: ClassVar[dict] = ...
    def __init__(self, value: int) -> None: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class IOType:
    __members__: ClassVar[dict] = ...  # read-only
    Invalid: ClassVar[IOType] = ...
    Queue: ClassVar[IOType] = ...
    RandomAccess: ClassVar[IOType] = ...
    __entries: ClassVar[dict] = ...
    def __init__(self, value: int) -> None: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class OpModelDesc:
    approx_mode: bool
    arch: str
    data_format: pybuda._C.DataFormat
    math_fidelity: Incomplete
    mblock_k: int
    mblock_m: int
    mblock_n: int
    op_attr: str
    reduce_z: int
    sparse_indices: int
    sparse_nz_strips: int
    sparse_nz_ublocks: int
    t: int
    type: str
    ublock_ct: int
    ublock_kt: int
    ublock_rt: int
    def __init__(self) -> None: ...

class PytorchTensorDesc:
    dim: int
    format: pybuda._C.DataFormat
    itemsize: int
    shape: List[int[4]]
    strides: List[int[4]]
    @overload
    def __init__(self) -> None: ...
    @overload
    def __init__(self, arg0: object, arg1: int, arg2: pybuda._C.DataFormat, arg3: int, arg4: List[int[4]], arg5: List[int[4]]) -> None: ...
    @overload
    def __init__(self, arg0: capsule, arg1: int, arg2: pybuda._C.DataFormat, arg3: int, arg4: List[int[4]], arg5: List[int[4]]) -> None: ...
    def print(self) -> None: ...

class StrideDescriptor:
    stride: int
    xy_offsets: List[Tuple[int, int]]
    def __init__(self) -> None: ...

class TilizedTensorDesc:
    buf_size_bytes: int
    format: pybuda._C.DataFormat
    num_buffers: int
    def __init__(self) -> None: ...
    def print(self) -> None: ...

@overload
def binarize_tensor(arg0: PytorchTensorDesc, arg1: str) -> None: ...
@overload
def binarize_tensor(arg0: TilizedTensorDesc, arg1: str) -> None: ...
def clear_backend_param_cache() -> None: ...
@overload
def debinarize_tensor(arg0: PytorchTensorDesc, arg1: str) -> None: ...
@overload
def debinarize_tensor(arg0: TilizedTensorDesc, arg1: str) -> None: ...
def detect_available_silicon_devices(only_detect_mmio: bool = ...) -> List[BackendDevice]: ...
def finish_child_process() -> BackendStatusCode: ...
@overload
def free_tensor(arg0: PytorchTensorDesc) -> BackendStatusCode: ...
@overload
def free_tensor(arg0: TilizedTensorDesc) -> BackendStatusCode: ...
def get_custom_device_desc(arch: BackendDevice = ..., mmio: bool = ..., harvesting_mask: int = ..., grid_dim: Tuple[int, int] = ..., out_dir: str = ...) -> BackendDeviceDesc: ...
def get_device_cluster_yaml(out_dir: str) -> str: ...
def get_device_descs_for_available_devices(out_dir: str = ...) -> List[BackendDeviceDesc]: ...
def get_golden_config() -> BackendConfig: ...
def get_io_size_in_bytes(data_formati: pybuda._C.DataFormat, is_untilizesd: bool, ublock_ct: int, ublock_rt: int, mblock_m: int, mblock_n: int, t: int, entries: int, tile_height: int = ..., tile_width: int = ...) -> int: ...
def get_next_aligned_address(arg0: int) -> int: ...
def get_op_model_execution_cycles(arg0: OpModelDesc) -> int: ...
def get_op_model_param(arg0: OpModelDesc, arg1: str) -> int: ...
def get_output(arg0: DramIODesc, arg1: PytorchTensorDesc, arg2: bool, arg3: int, arg4: int) -> BackendStatusCode: ...
def initialize_child_process(arg0: str) -> BackendStatusCode: ...
def load_cached_sys_param(arg0: str) -> Dict[str, str]: ...
def pop_output(arg0: DramIODesc, arg1: bool, arg2: int) -> BackendStatusCode: ...
@overload
def push_input(arg0: DramIODesc, arg1: PytorchTensorDesc, arg2: bool, arg3: int, arg4: int) -> BackendStatusCode: ...
@overload
def push_input(arg0: DramIODesc, arg1: TilizedTensorDesc, arg2: int, arg3: int) -> BackendStatusCode: ...
def release_backend_ptr(arg0: BackendApi) -> None: ...
def tilize_tensor(arg0: DramIODesc, arg1: PytorchTensorDesc) -> TilizedTensorDesc: ...
def translate_addresses(arg0: DramIODesc) -> BackendStatusCode: ...
