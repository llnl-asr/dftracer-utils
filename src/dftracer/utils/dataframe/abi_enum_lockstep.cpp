#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/types.h>

namespace dftracer::utils::dataframe {
namespace {

template <typename E>
constexpr std::int32_t code(E e) {
    return static_cast<std::int32_t>(e);
}

static_assert(code(Encoding::View) == 4);
static_assert(code(Encoding::Chunked) == 5);

static_assert(code(CmpOp::Gt) == DFTU_CMP_GT);
static_assert(code(CmpOp::Ge) == DFTU_CMP_GE);
static_assert(code(CmpOp::Lt) == DFTU_CMP_LT);
static_assert(code(CmpOp::Le) == DFTU_CMP_LE);
static_assert(code(CmpOp::Eq) == DFTU_CMP_EQ);
static_assert(code(CmpOp::Ne) == DFTU_CMP_NE);

static_assert(code(WindowFunc::RowNumber) == DFTU_WINDOW_ROW_NUMBER);
static_assert(code(WindowFunc::Rank) == DFTU_WINDOW_RANK);
static_assert(code(WindowFunc::DenseRank) == DFTU_WINDOW_DENSE_RANK);
static_assert(code(WindowFunc::Lag) == DFTU_WINDOW_LAG);
static_assert(code(WindowFunc::Lead) == DFTU_WINDOW_LEAD);
static_assert(code(WindowFunc::RunningSum) == DFTU_WINDOW_RUNNING_SUM);
static_assert(code(WindowFunc::RunningMin) == DFTU_WINDOW_RUNNING_MIN);
static_assert(code(WindowFunc::RunningMax) == DFTU_WINDOW_RUNNING_MAX);
static_assert(code(WindowFunc::RunningCount) == DFTU_WINDOW_RUNNING_COUNT);
static_assert(code(WindowFunc::Delta) == DFTU_WINDOW_DELTA);
static_assert(code(WindowFunc::Rate) == DFTU_WINDOW_RATE);
static_assert(code(WindowFunc::Sessionize) == DFTU_WINDOW_SESSIONIZE);
static_assert(code(WindowFunc::FrameSum) == DFTU_WINDOW_FRAME_SUM);
static_assert(code(WindowFunc::FrameMin) == DFTU_WINDOW_FRAME_MIN);
static_assert(code(WindowFunc::FrameMax) == DFTU_WINDOW_FRAME_MAX);
static_assert(code(WindowFunc::FrameCount) == DFTU_WINDOW_FRAME_COUNT);
static_assert(code(WindowFunc::FrameMean) == DFTU_WINDOW_FRAME_MEAN);
static_assert(code(WindowFunc::Ntile) == DFTU_WINDOW_NTILE);
static_assert(code(WindowFunc::FirstValue) == DFTU_WINDOW_FIRST_VALUE);
static_assert(code(WindowFunc::LastValue) == DFTU_WINDOW_LAST_VALUE);
static_assert(code(WindowFunc::NthValue) == DFTU_WINDOW_NTH_VALUE);
static_assert(code(WindowFunc::PercentRank) == DFTU_WINDOW_PERCENT_RANK);
static_assert(code(WindowFunc::CumeDist) == DFTU_WINDOW_CUME_DIST);
static_assert(code(WindowFunc::FillForward) == DFTU_WINDOW_FILL_FORWARD);
static_assert(code(WindowFunc::RunningProd) == DFTU_WINDOW_RUNNING_PROD);
static_assert(code(WindowFunc::FrameVar) == DFTU_WINDOW_FRAME_VAR);
static_assert(code(WindowFunc::FrameStd) == DFTU_WINDOW_FRAME_STD);
static_assert(code(WindowFunc::FrameQuantile) == DFTU_WINDOW_FRAME_QUANTILE);
static_assert(code(WindowFunc::FrameCountDistinct) ==
              DFTU_WINDOW_FRAME_COUNT_DISTINCT);
static_assert(code(WindowFunc::FrameArgMax) == DFTU_WINDOW_FRAME_ARG_MAX);
static_assert(code(WindowFunc::FrameArgMin) == DFTU_WINDOW_FRAME_ARG_MIN);
static_assert(code(WindowFunc::FrameCollect) == DFTU_WINDOW_FRAME_COLLECT);

static_assert(code(WindowFrameMode::Rows) == DFTU_WINDOW_FRAME_ROWS);
static_assert(code(WindowFrameMode::Range) == DFTU_WINDOW_FRAME_RANGE);

static_assert(code(GapFillMode::None) == DFTU_GAP_FILL_NONE);
static_assert(code(GapFillMode::Locf) == DFTU_GAP_FILL_LOCF);
static_assert(code(GapFillMode::Linear) == DFTU_GAP_FILL_LINEAR);

static_assert(code(AsofDirection::Backward) == DFTU_ASOF_BACKWARD);
static_assert(code(AsofDirection::Forward) == DFTU_ASOF_FORWARD);
static_assert(code(AsofDirection::Nearest) == DFTU_ASOF_NEAREST);

static_assert(code(LogicalOp::And) == DFTU_LOGICAL_AND);
static_assert(code(LogicalOp::Or) == DFTU_LOGICAL_OR);

static_assert(code(RankMethod::Average) == DFTU_RANK_AVERAGE);
static_assert(code(RankMethod::Min) == DFTU_RANK_MIN);
static_assert(code(RankMethod::Dense) == DFTU_RANK_DENSE);
static_assert(code(RankMethod::Ordinal) == DFTU_RANK_ORDINAL);
static_assert(code(RankMethod::Max) == DFTU_RANK_MAX);

static_assert(code(RollingOp::Sum) == DFTU_ROLLING_SUM);
static_assert(code(RollingOp::Mean) == DFTU_ROLLING_MEAN);
static_assert(code(RollingOp::Min) == DFTU_ROLLING_MIN);
static_assert(code(RollingOp::Max) == DFTU_ROLLING_MAX);

static_assert(code(StrPredOp::Contains) == DFTU_STR_PRED_CONTAINS);
static_assert(code(StrPredOp::StartsWith) == DFTU_STR_PRED_STARTS_WITH);
static_assert(code(StrPredOp::EndsWith) == DFTU_STR_PRED_ENDS_WITH);
static_assert(code(StrPredOp::Like) == DFTU_STR_PRED_LIKE);
static_assert(code(StrPredOp::Matches) == DFTU_STR_PRED_MATCHES);
static_assert(code(StrPredOp::Search) == DFTU_STR_PRED_SEARCH);

static_assert(code(StrMapOp::Lower) == DFTU_STR_MAP_LOWER);
static_assert(code(StrMapOp::Upper) == DFTU_STR_MAP_UPPER);
static_assert(code(StrMapOp::Strip) == DFTU_STR_MAP_STRIP);
static_assert(code(StrMapOp::Lstrip) == DFTU_STR_MAP_LSTRIP);
static_assert(code(StrMapOp::Rstrip) == DFTU_STR_MAP_RSTRIP);
static_assert(code(StrMapOp::Capitalize) == DFTU_STR_MAP_CAPITALIZE);
static_assert(code(StrMapOp::Title) == DFTU_STR_MAP_TITLE);
static_assert(code(StrMapOp::Swapcase) == DFTU_STR_MAP_SWAPCASE);

static_assert(code(StrFn::IsAlnum) == DFTU_STR_FN_ISALNUM);
static_assert(code(StrFn::IsAlpha) == DFTU_STR_FN_ISALPHA);
static_assert(code(StrFn::IsDigit) == DFTU_STR_FN_ISDIGIT);
static_assert(code(StrFn::IsDecimal) == DFTU_STR_FN_ISDECIMAL);
static_assert(code(StrFn::IsNumeric) == DFTU_STR_FN_ISNUMERIC);
static_assert(code(StrFn::IsSpace) == DFTU_STR_FN_ISSPACE);
static_assert(code(StrFn::IsLower) == DFTU_STR_FN_ISLOWER);
static_assert(code(StrFn::IsUpper) == DFTU_STR_FN_ISUPPER);
static_assert(code(StrFn::IsTitle) == DFTU_STR_FN_ISTITLE);
static_assert(code(StrFn::PadStart) == DFTU_STR_FN_PAD_START);
static_assert(code(StrFn::PadEnd) == DFTU_STR_FN_PAD_END);
static_assert(code(StrFn::Center) == DFTU_STR_FN_CENTER);
static_assert(code(StrFn::Zfill) == DFTU_STR_FN_ZFILL);
static_assert(code(StrFn::RemovePrefix) == DFTU_STR_FN_REMOVE_PREFIX);
static_assert(code(StrFn::RemoveSuffix) == DFTU_STR_FN_REMOVE_SUFFIX);
static_assert(code(StrFn::Repeat) == DFTU_STR_FN_REPEAT);
static_assert(code(StrFn::SliceReplace) == DFTU_STR_FN_SLICE_REPLACE);
static_assert(code(StrFn::Split) == DFTU_STR_FN_SPLIT);
static_assert(code(StrFn::Partition) == DFTU_STR_FN_PARTITION);
static_assert(code(StrFn::RPartition) == DFTU_STR_FN_RPARTITION);
static_assert(code(StrFn::Findall) == DFTU_STR_FN_FINDALL);
static_assert(code(StrFn::Extract) == DFTU_STR_FN_EXTRACT);
static_assert(code(StrFn::Rfind) == DFTU_STR_FN_RFIND);
static_assert(code(StrFn::Index) == DFTU_STR_FN_INDEX);
static_assert(code(StrFn::Rindex) == DFTU_STR_FN_RINDEX);
static_assert(code(StrFn::Join) == DFTU_STR_FN_JOIN);
static_assert(code(StrFn::Get) == DFTU_STR_FN_GET);
static_assert(code(StrFn::Cat) == DFTU_STR_FN_CAT);
static_assert(code(StrFn::RegexReplace) == DFTU_STR_FN_REGEX_REPLACE);

static_assert(code(ConcatHow::Vertical) == DFTU_CONCAT_VERTICAL);
static_assert(code(ConcatHow::Diagonal) == DFTU_CONCAT_DIAGONAL);

static_assert(code(JoinHow::Inner) == DFTU_JOIN_INNER);
static_assert(code(JoinHow::Left) == DFTU_JOIN_LEFT);
static_assert(code(JoinHow::Right) == DFTU_JOIN_RIGHT);
static_assert(code(JoinHow::Outer) == DFTU_JOIN_OUTER);
static_assert(code(JoinHow::Semi) == DFTU_JOIN_SEMI);
static_assert(code(JoinHow::Anti) == DFTU_JOIN_ANTI);
static_assert(code(JoinHow::Cross) == DFTU_JOIN_CROSS);
static_assert(code(JoinHow::Lookup) == DFTU_JOIN_LOOKUP);
static_assert(code(JoinHow::Nest) == DFTU_JOIN_NEST);

static_assert(code(PrimOp::Ilog2) == DFTU_PRIM_ILOG2);
static_assert(code(PrimOp::BitWidth) == DFTU_PRIM_BIT_WIDTH);
static_assert(code(PrimOp::Popcount) == DFTU_PRIM_POPCOUNT);
static_assert(code(PrimOp::Clz) == DFTU_PRIM_CLZ);
static_assert(code(PrimOp::Ctz) == DFTU_PRIM_CTZ);
static_assert(code(PrimOp::Mix64) == DFTU_PRIM_MIX64);

static_assert(code(ScalarTag::I64) == DFTU_SCALAR_TAG_I64);
static_assert(code(ScalarTag::U64) == DFTU_SCALAR_TAG_U64);
static_assert(code(ScalarTag::F64) == DFTU_SCALAR_TAG_F64);
static_assert(code(ScalarTag::Str) == DFTU_SCALAR_TAG_STR);
static_assert(code(ScalarTag::Err) == DFTU_SCALAR_TAG_ERR);

// Every TypeId mirrors a dftu_dtype, in lockstep, so a plugin can name any
// type a Series can hold through the C ABI.
static_assert(code(TypeId::Unknown) == DFTU_TYPE_UNKNOWN);
static_assert(code(TypeId::Bool) == DFTU_TYPE_BOOL);
static_assert(code(TypeId::Int8) == DFTU_TYPE_INT8);
static_assert(code(TypeId::Int16) == DFTU_TYPE_INT16);
static_assert(code(TypeId::Int32) == DFTU_TYPE_INT32);
static_assert(code(TypeId::Int64) == DFTU_TYPE_INT64);
static_assert(code(TypeId::Uint8) == DFTU_TYPE_UINT8);
static_assert(code(TypeId::Uint16) == DFTU_TYPE_UINT16);
static_assert(code(TypeId::Uint32) == DFTU_TYPE_UINT32);
static_assert(code(TypeId::Uint64) == DFTU_TYPE_UINT64);
static_assert(code(TypeId::Float32) == DFTU_TYPE_FLOAT32);
static_assert(code(TypeId::Float64) == DFTU_TYPE_FLOAT64);
static_assert(code(TypeId::String) == DFTU_TYPE_STRING);
static_assert(code(TypeId::Binary) == DFTU_TYPE_BINARY);
static_assert(code(TypeId::List) == DFTU_TYPE_LIST);
static_assert(code(TypeId::Struct) == DFTU_TYPE_STRUCT);
static_assert(code(TypeId::Float16) == DFTU_TYPE_FLOAT16);
static_assert(code(TypeId::Date32) == DFTU_TYPE_DATE32);
static_assert(code(TypeId::Date64) == DFTU_TYPE_DATE64);
static_assert(code(TypeId::Time32) == DFTU_TYPE_TIME32);
static_assert(code(TypeId::Time64) == DFTU_TYPE_TIME64);
static_assert(code(TypeId::Timestamp) == DFTU_TYPE_TIMESTAMP);
static_assert(code(TypeId::Duration) == DFTU_TYPE_DURATION);
static_assert(code(TypeId::Decimal128) == DFTU_TYPE_DECIMAL128);
static_assert(code(TypeId::Decimal256) == DFTU_TYPE_DECIMAL256);
static_assert(code(TypeId::FixedSizeBinary) == DFTU_TYPE_FIXED_SIZE_BINARY);
static_assert(code(TypeId::LargeString) == DFTU_TYPE_LARGE_STRING);
static_assert(code(TypeId::LargeBinary) == DFTU_TYPE_LARGE_BINARY);
static_assert(code(TypeId::LargeList) == DFTU_TYPE_LARGE_LIST);
static_assert(code(TypeId::FixedSizeList) == DFTU_TYPE_FIXED_SIZE_LIST);
static_assert(code(TypeId::Map) == DFTU_TYPE_MAP);
// TypeId::Map is declared last in types.h, so its ordinal is the count of
// TypeId values minus one; pinning DFTU_TYPE_MAP to that ordinal makes a
// TypeId appended without a matching dftu_dtype member fail this assert
// instead of silently reusing 30.
static_assert(DFTU_TYPE_MAP == 30,
              "a TypeId was added without mirroring it into dftu_dtype");

static_assert(code(TimeUnit::Second) == DFTU_TIME_UNIT_SECOND);
static_assert(code(TimeUnit::Milli) == DFTU_TIME_UNIT_MILLI);
static_assert(code(TimeUnit::Micro) == DFTU_TIME_UNIT_MICRO);
static_assert(code(TimeUnit::Nano) == DFTU_TIME_UNIT_NANO);

}  // namespace
}  // namespace dftracer::utils::dataframe
