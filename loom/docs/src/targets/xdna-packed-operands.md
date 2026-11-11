# Pre-encoded matrix operands on XDNA

Host code can prepare AIE2P matrix weights as packed bytes and load them directly
into a matrix fragment. This avoids encoding the same weights inside each
invocation. The layout below applies to `bfp16ebs8` with
`payload_packing=target_fragment` on `amd.xdna.aie2p`.

## Pack an 8 × 8 operand

One operand contains 64 values in 72 bytes: eight consecutive rows, each with
one shared exponent byte followed by eight signed mantissa bytes. There is no
padding between rows or consecutive operands.

| Byte offset within an operand | Contents |
| --- | --- |
| `9 * row` | Unsigned shared exponent for that row. |
| `9 * row + 1 + column` | Signed, two's-complement 8-bit mantissa. |

For finite values, decode a row element as
`mantissa * 2 ** (exponent - 133)`. For example, exponent 133 makes mantissas
`-3` and `5` represent `-3.0` and `5.0`; exponent 134 doubles both values.
The exponent byte 133 is `0x85`, while the mantissa -3 is `0xfd`.
This byte layout describes storage, not a choice of quantization algorithm or
an error guarantee for converting model weights. A host encoder must choose
shared exponents and mantissas appropriate to its numerical contract.

Both operands use this row layout. The native matrix operation computes
`A * B^T`: row `n` of the RHS operand contains the eight reduction elements for
output column `n`. Pack each RHS block as `[output column][reduction element]`,
rather than as the corresponding rows of an ordinary row-major `B` matrix.

The schema for an 8 × 8 operand is:

```loom
%schema = encoding.define #encoding.operand<element_format=bfp16ebs8, payload_elements=64, payload_packing=target_fragment, payload_registers=18, rounding=flush_subnormal> : encoding<schema>
```

`payload_elements=64` counts logical values; `payload_registers=18` describes
the 72-byte payload in 32-bit units. Neither field adds padding to the byte
stream.

## Load consecutive blocks as a fragment group

A group of eight operands occupies 576 bytes. Given a `view<576xi8>` named
`%weights` whose base satisfies the enclosing buffer's alignment contract, load
the group, attach its block shape, then slice a 72-byte operand:

```loom
%blocks = index.constant 8 : index
%extent = index.constant 8 : index
%bytes = vector.load %weights[0] : view<576xi8> -> vector<576xi8>
%group = vector.fragment<rhs> %bytes shape [blocks(%blocks), %extent, %extent] using {schema = %schema : encoding<schema>} : vector<576xi8>
%first_bytes = vector.slice %group[0] : vector<576xi8> -> vector<72xi8>
%first = vector.fragment<rhs> %first_bytes shape [%extent, %extent] using {schema = %schema : encoding<schema>} : vector<72xi8>
```

Operand `j` starts at byte offset `72 * j`. Attach the fragment shape before
slicing so the compiler retains the matrix representation. A raw byte vector
and a fragment carrying those bytes have different register requirements.
Group size and the number of simultaneously live operands must still fit the
target's register budget.

The maintained [packed matrix execution test](https://github.com/ROCm/hrx-system/blob/main/loom/target/amd/xdna/test/native/packed_matrix_groups.loom)
checks signed mantissas, distinct row exponents, RHS orientation, and slices
across group boundaries against an independent scalar decoder. For data
movement between host buffers and resident workers, see
[resident pipelines on XDNA](xdna-pipelines.md).
