#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "soia.h"
#include "soiagen/goldens.h"

namespace {

using ::soiagen_goldens::Assertion;
using ::soiagen_goldens::BytesExpression;
using ::soiagen_goldens::StringExpression;
using ::soiagen_goldens::UnitTest;

class TypedValue {
 public:
  virtual ~TypedValue() = default;

  virtual std::string ToDenseJson() const = 0;
  virtual std::string ToReadableJson() const = 0;
  virtual soia::ByteString ToBytes() const = 0;
  virtual absl::StatusOr<std::unique_ptr<TypedValue>> RoundTripDenseJson()
      const = 0;
  virtual absl::StatusOr<std::unique_ptr<TypedValue>> RoundTripReadableJson()
      const = 0;
  virtual absl::StatusOr<std::unique_ptr<TypedValue>> RoundTripBytes()
      const = 0;

  virtual void CheckParse(absl::string_view bytes_or_json) const = 0;
};

template <typename T>
class TypedValueImpl : public TypedValue {
 public:
  explicit TypedValueImpl(const T& value) : value_(value) {}

  std::string ToDenseJson() const override { return soia::ToDenseJson(value_); }

  std::string ToReadableJson() const override {
    return soia::ToReadableJson(value_);
  }

  soia::ByteString ToBytes() const override { return soia::ToBytes(value_); }

  absl::StatusOr<std::unique_ptr<TypedValue>> RoundTripDenseJson()
      const override {
    const absl::StatusOr<T> parse_result = soia::Parse<T>(ToDenseJson());
    if (!parse_result.ok()) {
      return parse_result.status();
    }
    return std::make_unique<TypedValueImpl<T>>(*parse_result);
  }

  absl::StatusOr<std::unique_ptr<TypedValue>> RoundTripReadableJson()
      const override {
    const absl::StatusOr<T> parse_result = soia::Parse<T>(ToReadableJson());
    if (!parse_result.ok()) {
      return parse_result.status();
    }
    return std::make_unique<TypedValueImpl<T>>(*parse_result);
  }

  absl::StatusOr<std::unique_ptr<TypedValue>> RoundTripBytes() const override {
    const absl::StatusOr<T> parse_result =
        soia::Parse<T>(ToBytes().as_string());
    if (!parse_result.ok()) {
      return parse_result.status();
    }
    return std::make_unique<TypedValueImpl<T>>(*parse_result);
  }

  void CheckParse(absl::string_view bytes_or_json) const override {
    const absl::StatusOr<T> parse_result = soia::Parse<T>(bytes_or_json);
    EXPECT_EQ(parse_result.status(), absl::OkStatus());
    if (!parse_result.ok()) {
      return;
    }
    EXPECT_EQ(*parse_result, value_);
  }

 private:
  T value_;
};

absl::StatusOr<std::string> EvalStringExpression(const StringExpression& expr);

absl::StatusOr<soia::ByteString> EvalBytesExpression(
    const BytesExpression& expr);

absl::StatusOr<std::unique_ptr<TypedValue>> EvalTypedValue(
    const soiagen_goldens::TypedValue& typed_value) {
  switch (typed_value.kind()) {
    case soiagen_goldens::TypedValue::kind_type::kValBool:
      return std::make_unique<TypedValueImpl<bool>>(typed_value.as_bool());
    case soiagen_goldens::TypedValue::kind_type::kValInt32:
      return std::make_unique<TypedValueImpl<int32_t>>(typed_value.as_int32());
    case soiagen_goldens::TypedValue::kind_type::kValInt64:
      return std::make_unique<TypedValueImpl<int64_t>>(typed_value.as_int64());
    case soiagen_goldens::TypedValue::kind_type::kValUint64:
      return std::make_unique<TypedValueImpl<uint64_t>>(
          typed_value.as_uint64());
    case soiagen_goldens::TypedValue::kind_type::kValFloat32:
      return std::make_unique<TypedValueImpl<float>>(typed_value.as_float32());
    case soiagen_goldens::TypedValue::kind_type::kValFloat64:
      return std::make_unique<TypedValueImpl<double>>(typed_value.as_float64());
    case soiagen_goldens::TypedValue::kind_type::kValTimestamp:
      return std::make_unique<TypedValueImpl<absl::Time>>(
          typed_value.as_timestamp());
    case soiagen_goldens::TypedValue::kind_type::kValString:
      return std::make_unique<TypedValueImpl<std::string>>(
          typed_value.as_string());
    case soiagen_goldens::TypedValue::kind_type::kValBytes:
      return std::make_unique<TypedValueImpl<soia::ByteString>>(
          soia::ByteString(typed_value.as_bytes()));
    case soiagen_goldens::TypedValue::kind_type::kValBoolOptional:
      return std::make_unique<TypedValueImpl<absl::optional<bool>>>(
          typed_value.as_bool_optional());
    case soiagen_goldens::TypedValue::kind_type::kValInts:
      return std::make_unique<TypedValueImpl<std::vector<int32_t>>>(
          typed_value.as_ints());
    case soiagen_goldens::TypedValue::kind_type::kValPoint:
      return std::make_unique<TypedValueImpl<soiagen_goldens::Point>>(
          typed_value.as_point());
    case soiagen_goldens::TypedValue::kind_type::kValColor:
      return std::make_unique<TypedValueImpl<soiagen_goldens::Color>>(
          typed_value.as_color());
    case soiagen_goldens::TypedValue::kind_type::kValMyEnum:
      return std::make_unique<TypedValueImpl<soiagen_goldens::MyEnum>>(
          typed_value.as_my_enum());
    case soiagen_goldens::TypedValue::kind_type::kValRoundTripDenseJson: {
      const absl::StatusOr<std::unique_ptr<TypedValue>> other =
          EvalTypedValue(typed_value.as_round_trip_dense_json());
      if (!other.ok()) {
        return other.status();
      }
      return (*other)->RoundTripDenseJson();
    }
    case soiagen_goldens::TypedValue::kind_type::kValRoundTripReadableJson: {
      const absl::StatusOr<std::unique_ptr<TypedValue>> other =
          EvalTypedValue(typed_value.as_round_trip_readable_json());
      if (!other.ok()) {
        return other.status();
      }
      return (*other)->RoundTripReadableJson();
    }
    case soiagen_goldens::TypedValue::kind_type::kValRoundTripBytes: {
      const absl::StatusOr<std::unique_ptr<TypedValue>> other =
          EvalTypedValue(typed_value.as_round_trip_bytes());
      if (!other.ok()) {
        return other.status();
      }
      return (*other)->RoundTripBytes();
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValPointFromJsonKeepUnrecognized: {
      const absl::StatusOr<std::string> string_expression =
          EvalStringExpression(
              typed_value.as_point_from_json_keep_unrecognized());
      if (!string_expression.ok()) {
        return string_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Point> point =
          soia::Parse<soiagen_goldens::Point>(
              *string_expression, soia::UnrecognizedFieldsPolicy::kKeep);
      if (!point.ok()) {
        return point.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Point>>(*point);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValPointFromJsonDropUnrecognized: {
      const absl::StatusOr<std::string> string_expression =
          EvalStringExpression(
              typed_value.as_point_from_json_drop_unrecognized());
      if (!string_expression.ok()) {
        return string_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Point> point =
          soia::Parse<soiagen_goldens::Point>(
              *string_expression, soia::UnrecognizedFieldsPolicy::kDrop);
      if (!point.ok()) {
        return point.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Point>>(*point);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValPointFromBytesKeepUnrecognized: {
      const absl::StatusOr<soia::ByteString> bytes_expression =
          EvalBytesExpression(
              typed_value.as_point_from_bytes_keep_unrecognized());
      if (!bytes_expression.ok()) {
        return bytes_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Point> point =
          soia::Parse<soiagen_goldens::Point>(
              bytes_expression->as_string(),
              soia::UnrecognizedFieldsPolicy::kKeep);
      if (!point.ok()) {
        return point.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Point>>(*point);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValPointFromBytesDropUnrecognized: {
      const absl::StatusOr<soia::ByteString> bytes_expression =
          EvalBytesExpression(
              typed_value.as_point_from_bytes_drop_unrecognized());
      if (!bytes_expression.ok()) {
        return bytes_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Point> point =
          soia::Parse<soiagen_goldens::Point>(
              bytes_expression->as_string(),
              soia::UnrecognizedFieldsPolicy::kDrop);
      if (!point.ok()) {
        return point.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Point>>(*point);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValColorFromJsonKeepUnrecognized: {
      const absl::StatusOr<std::string> string_expression =
          EvalStringExpression(
              typed_value.as_color_from_json_keep_unrecognized());
      if (!string_expression.ok()) {
        return string_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Color> color =
          soia::Parse<soiagen_goldens::Color>(
              *string_expression, soia::UnrecognizedFieldsPolicy::kKeep);
      if (!color.ok()) {
        return color.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Color>>(*color);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValColorFromJsonDropUnrecognized: {
      const absl::StatusOr<std::string> string_expression =
          EvalStringExpression(
              typed_value.as_color_from_json_drop_unrecognized());
      if (!string_expression.ok()) {
        return string_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Color> color =
          soia::Parse<soiagen_goldens::Color>(
              *string_expression, soia::UnrecognizedFieldsPolicy::kDrop);
      if (!color.ok()) {
        return color.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Color>>(*color);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValColorFromBytesKeepUnrecognized: {
      const absl::StatusOr<soia::ByteString> bytes_expression =
          EvalBytesExpression(
              typed_value.as_color_from_bytes_keep_unrecognized());
      if (!bytes_expression.ok()) {
        return bytes_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Color> color =
          soia::Parse<soiagen_goldens::Color>(
              bytes_expression->as_string(),
              soia::UnrecognizedFieldsPolicy::kKeep);
      if (!color.ok()) {
        return color.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Color>>(*color);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValColorFromBytesDropUnrecognized: {
      const absl::StatusOr<soia::ByteString> bytes_expression =
          EvalBytesExpression(
              typed_value.as_color_from_bytes_drop_unrecognized());
      if (!bytes_expression.ok()) {
        return bytes_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::Color> color =
          soia::Parse<soiagen_goldens::Color>(
              bytes_expression->as_string(),
              soia::UnrecognizedFieldsPolicy::kDrop);
      if (!color.ok()) {
        return color.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::Color>>(*color);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValMyEnumFromJsonKeepUnrecognized: {
      const absl::StatusOr<std::string> string_expression =
          EvalStringExpression(
              typed_value.as_my_enum_from_json_keep_unrecognized());
      if (!string_expression.ok()) {
        return string_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::MyEnum> my_enum =
          soia::Parse<soiagen_goldens::MyEnum>(
              *string_expression, soia::UnrecognizedFieldsPolicy::kKeep);
      if (!my_enum.ok()) {
        return my_enum.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::MyEnum>>(
          *my_enum);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValMyEnumFromJsonDropUnrecognized: {
      const absl::StatusOr<std::string> string_expression =
          EvalStringExpression(
              typed_value.as_my_enum_from_json_drop_unrecognized());
      if (!string_expression.ok()) {
        return string_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::MyEnum> my_enum =
          soia::Parse<soiagen_goldens::MyEnum>(
              *string_expression, soia::UnrecognizedFieldsPolicy::kDrop);
      if (!my_enum.ok()) {
        return my_enum.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::MyEnum>>(
          *my_enum);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValMyEnumFromBytesKeepUnrecognized: {
      const absl::StatusOr<soia::ByteString> bytes_expression =
          EvalBytesExpression(
              typed_value.as_my_enum_from_bytes_keep_unrecognized());
      if (!bytes_expression.ok()) {
        return bytes_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::MyEnum> my_enum =
          soia::Parse<soiagen_goldens::MyEnum>(
              bytes_expression->as_string(),
              soia::UnrecognizedFieldsPolicy::kKeep);
      if (!my_enum.ok()) {
        return my_enum.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::MyEnum>>(
          *my_enum);
    }
    case soiagen_goldens::TypedValue::kind_type::
        kValMyEnumFromBytesDropUnrecognized: {
      const absl::StatusOr<soia::ByteString> bytes_expression =
          EvalBytesExpression(
              typed_value.as_my_enum_from_bytes_drop_unrecognized());
      if (!bytes_expression.ok()) {
        return bytes_expression.status();
      }
      const absl::StatusOr<soiagen_goldens::MyEnum> my_enum =
          soia::Parse<soiagen_goldens::MyEnum>(
              bytes_expression->as_string(),
              soia::UnrecognizedFieldsPolicy::kDrop);
      if (!my_enum.ok()) {
        return my_enum.status();
      }
      return std::make_unique<TypedValueImpl<soiagen_goldens::MyEnum>>(
          *my_enum);
    }
    case soiagen_goldens::TypedValue::kind_type::kConstUnknown:
      return absl::InvalidArgumentError("Unknown TypedValue kind");
  }
}

absl::StatusOr<std::string> EvalStringExpression(const StringExpression& expr) {
  switch (expr.kind()) {
    case StringExpression::kind_type::kValLiteral:
      return expr.as_literal();
    case StringExpression::kind_type::kValToDenseJson: {
      const absl::StatusOr<std::unique_ptr<TypedValue>> typed_value =
          EvalTypedValue(expr.as_to_dense_json());
      if (!typed_value.ok()) {
        return typed_value.status();
      }
      return (*typed_value)->ToDenseJson();
    }
    case StringExpression::kind_type::kValToReadableJson: {
      const absl::StatusOr<std::unique_ptr<TypedValue>> typed_value =
          EvalTypedValue(expr.as_to_readable_json());
      if (!typed_value.ok()) {
        return typed_value.status();
      }
      return (*typed_value)->ToReadableJson();
    }
    case StringExpression::kind_type::kConstUnknown: {
      return absl::InvalidArgumentError("Unknown StringExpression kind");
    }
  }
}

// Helper function to evaluate a BytesExpression
absl::StatusOr<soia::ByteString> EvalBytesExpression(
    const BytesExpression& expr) {
  switch (expr.kind()) {
    case BytesExpression::kind_type::kValLiteral:
      return expr.as_literal();
    case BytesExpression::kind_type::kValToBytes: {
      const absl::StatusOr<std::unique_ptr<TypedValue>> typed_value =
          EvalTypedValue(expr.as_to_bytes());
      if (!typed_value.ok()) {
        return typed_value.status();
      }
      return (*typed_value)->ToBytes();
    }
    case BytesExpression::kind_type::kConstUnknown: {
      return absl::InvalidArgumentError("Unknown BytesExpression kind");
    }
  }
}

// Helper to execute ReserializeValue assertion
void ExecuteReserializeValue(const Assertion::ReserializeValue& assertion) {
  const absl::StatusOr<std::unique_ptr<TypedValue>> typed_value =
      EvalTypedValue(assertion.value);
  EXPECT_EQ(typed_value.status(), absl::OkStatus());
  if (!typed_value.ok()) {
    return;
  }

  // Check dense JSON serialization
  {
    const std::string actual_dense_json = (*typed_value)->ToDenseJson();
    bool dense_json_matched = false;
    for (const auto& expected_json : assertion.expected_dense_json) {
      if (actual_dense_json == expected_json) {
        dense_json_matched = true;
        break;
      }
    }
    EXPECT_TRUE(dense_json_matched)
        << "Dense JSON mismatch. Actual: " << actual_dense_json
        << ", Expected one of: " << assertion.expected_dense_json.size()
        << " values";
  }

  // Check readable JSON serialization
  {
    const std::string actual_readable_json = (*typed_value)->ToReadableJson();
    bool readable_json_matched = false;
    for (const auto& expected_json : assertion.expected_readable_json) {
      if (actual_readable_json == expected_json) {
        readable_json_matched = true;
        break;
      }
    }
    EXPECT_TRUE(readable_json_matched)
        << "Readable JSON mismatch. Actual: " << actual_readable_json
        << ", Expected one of: " << assertion.expected_readable_json.size()
        << " values";
  }

  // Check deserialization
  {
    for (const auto& expected_json : assertion.expected_dense_json) {
      (*typed_value)->CheckParse(expected_json);
    }
    for (const auto& expected_json : assertion.expected_readable_json) {
      (*typed_value)->CheckParse(expected_json);
    }
    for (const auto& expected_bytes : assertion.expected_bytes) {
      (*typed_value)->CheckParse(expected_bytes.as_string());
    }
    for (const auto& alternative_json : assertion.alternative_jsons) {
      const absl::StatusOr<std::string> json =
          EvalStringExpression(alternative_json);
      EXPECT_EQ(json.status(), absl::OkStatus());
      if (json.ok()) {
        (*typed_value)->CheckParse(*json);
      }
    }
    for (const auto& alternative_bytes : assertion.alternative_bytes) {
      const absl::StatusOr<soia::ByteString> bytes =
          EvalBytesExpression(alternative_bytes);
      EXPECT_EQ(bytes.status(), absl::OkStatus());
      if (bytes.ok()) {
        (*typed_value)->CheckParse(bytes->as_string());
      }
    }
  }

  // Check bytes serialization
  {
    const soia::ByteString actual_bytes = (*typed_value)->ToBytes();
    bool bytes_matched = false;
    for (const auto& expected_bytes : assertion.expected_bytes) {
      if (actual_bytes == expected_bytes) {
        bytes_matched = true;
        break;
      }
    }
    EXPECT_TRUE(bytes_matched)
        << "Bytes serialization mismatch; actual: "
        << absl::BytesToHexString(actual_bytes.as_string());
  }

  // Make sure the encoded value can be skipped.
  for (const auto& expected_bytes : assertion.expected_bytes) {
    const soia::ByteString bytes = soia::ByteString(
        absl::StrCat("soia\xF8", expected_bytes.as_string().substr(4), "\x01"));
    const absl::StatusOr<soiagen_goldens::Point> point =
        soia::Parse<soiagen_goldens::Point>(bytes.as_string());
    EXPECT_EQ(point.status(), absl::OkStatus());
    if (point.ok()) {
      EXPECT_EQ(point->x, 1)
          << "Failed to skip value; input: "
          << absl::BytesToHexString(expected_bytes.as_string());
    }
  }
}

// Helper to execute ReserializeLargeString assertion
void ExecuteReserializeLargeString(
    const Assertion::ReserializeLargeString& assertion) {
  // Create a large string with num_chars characters
  const std::string large_string(assertion.num_chars, 'a');

  // Serialize to bytes
  const soia::ByteString serialized = soia::ToBytes(large_string);

  // Check that it starts with the expected prefix
  const soia::ByteString& expected_prefix = assertion.expected_byte_prefix;

  EXPECT_GE(serialized.as_string().length(),
            expected_prefix.as_string().length())
      << "Serialized bytes too short";

  const std::string actual_prefix = std::string(
      serialized.as_string().substr(0, expected_prefix.as_string().length()));
  EXPECT_EQ(actual_prefix, expected_prefix.as_string())
      << "Prefix mismatch for large string";
}

// Helper to execute ReserializeLargeArray assertion
void ExecuteReserializeLargeArray(
    const Assertion::ReserializeLargeArray& assertion) {
  // Create a large array with num_items elements
  std::vector<int32_t> large_array(assertion.num_items, 0);

  // Serialize to bytes
  soia::ByteString serialized = soia::ToBytes(large_array);

  // Check that it starts with the expected prefix
  const soia::ByteString expected_prefix = assertion.expected_byte_prefix;

  EXPECT_GE(serialized.as_string().length(),
            expected_prefix.as_string().length())
      << "Serialized bytes too short";

  std::string actual_prefix = std::string(
      serialized.as_string().substr(0, expected_prefix.as_string().length()));
  EXPECT_EQ(actual_prefix, expected_prefix.as_string())
      << "Prefix mismatch for large array";
}

void ExecuteBytesEqual(const Assertion::BytesEqual& assertion) {
  const absl::StatusOr<soia::ByteString> actual =
      EvalBytesExpression(assertion.actual);
  const absl::StatusOr<soia::ByteString> expected =
      EvalBytesExpression(assertion.expected);
  EXPECT_EQ(actual.status(), absl::OkStatus());
  EXPECT_EQ(expected.status(), absl::OkStatus());
  if (actual.ok() && expected.ok()) {
    EXPECT_EQ(*actual, *expected) << "Bytes not equal";
  }
}

void ExecuteBytesIn(const Assertion::BytesIn& assertion) {
  const absl::StatusOr<soia::ByteString> actual =
      EvalBytesExpression(assertion.actual);
  EXPECT_EQ(actual.status(), absl::OkStatus());
  if (!actual.ok()) {
    return;
  }
  bool found = false;
  for (const auto& expected : assertion.expected) {
    if (*actual == expected) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Bytes not in expected set";
}

void ExecuteStringEqual(const Assertion::StringEqual& assertion) {
  const absl::StatusOr<std::string> actual =
      EvalStringExpression(assertion.actual);
  const absl::StatusOr<std::string> expected =
      EvalStringExpression(assertion.expected);
  EXPECT_EQ(actual.status(), absl::OkStatus());
  EXPECT_EQ(expected.status(), absl::OkStatus());
  if (actual.ok() && expected.ok()) {
    EXPECT_EQ(*actual, *expected) << "Strings not equal";
  }
}

void ExecuteStringIn(const Assertion::StringIn& assertion) {
  const absl::StatusOr<std::string> actual =
      EvalStringExpression(assertion.actual);
  EXPECT_EQ(actual.status(), absl::OkStatus());
  if (!actual.ok()) {
    return;
  }
  bool found = false;
  for (const auto& expected : assertion.expected) {
    if (*actual == expected) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "String not in expected set";
}

// Main test that runs all unit tests
TEST(SoiaGoldensTest, AllTests) {
  const std::vector<UnitTest>& unit_tests = soiagen_goldens::k_unit_tests();

  for (const UnitTest& test : unit_tests) {
    SCOPED_TRACE(absl::StrCat("Test number: ", test.test_number));

    const Assertion& assertion = test.assertion;

    switch (assertion.kind()) {
      case Assertion::kind_type::kValReserializeValue:
        ExecuteReserializeValue(assertion.as_reserialize_value());
        break;

      case Assertion::kind_type::kValReserializeLargeString:
        ExecuteReserializeLargeString(assertion.as_reserialize_large_string());
        break;

      case Assertion::kind_type::kValReserializeLargeArray:
        ExecuteReserializeLargeArray(assertion.as_reserialize_large_array());
        break;

      case Assertion::kind_type::kValBytesEqual:
        ExecuteBytesEqual(assertion.as_bytes_equal());
        break;

      case Assertion::kind_type::kValBytesIn:
        ExecuteBytesIn(assertion.as_bytes_in());
        break;

      case Assertion::kind_type::kValStringEqual:
        ExecuteStringEqual(assertion.as_string_equal());
        break;

      case Assertion::kind_type::kValStringIn:
        ExecuteStringIn(assertion.as_string_in());
        break;

      default:
        FAIL() << "Unknown assertion kind";
    }
  }
}

}  // namespace
