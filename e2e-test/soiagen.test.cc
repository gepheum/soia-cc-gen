#include <gtest/gtest.h>

#include <type_traits>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "gmock/gmock.h"
#include "reserializer.testing.h"
#include "soia.h"
#include "soia.testing.h"
#include "soiagen/constants.h"
#include "soiagen/enums.h"
#include "soiagen/enums.testing.h"
#include "soiagen/full_name.h"
#include "soiagen/methods.h"
#include "soiagen/simple_enum.h"
#include "soiagen/simple_enum.testing.h"
#include "soiagen/structs.h"
#include "soiagen/structs.testing.h"

namespace {
using ::absl_testing::IsOk;
using ::absl_testing::IsOkAndHolds;
using ::soia_testing_internal::MakeReserializer;
using ::soiagen_enums::EmptyEnum;
using ::soiagen_enums::JsonValue;
using ::soiagen_enums::Weekday;
using ::soiagen_full_name::FullName;
using ::soiagen_structs::Bundle;
using ::soiagen_structs::CarOwner;
using ::soiagen_structs::Empty;
using ::soiagen_structs::EmptyWithRm1;
using ::soiagen_structs::Item;
using ::soiagen_structs::KeyedItems;
using ::soiagen_user::User;
using ::soiagen_vehicles_car::Car;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

using StatusEnum = ::soiagen_simple_enum::Status;

TEST(SoiagenTest, StructEqAndHash) {
  absl::flat_hash_set<FullName> full_names;
  EXPECT_TRUE(
      full_names.insert(FullName{.first_name = "Osi", .last_name = "Daro"})
          .second);
  EXPECT_FALSE(
      full_names.insert(FullName{.first_name = "Osi", .last_name = "Daro"})
          .second);
  EXPECT_TRUE(full_names.insert(FullName{.first_name = "Osi"}).second);
  EXPECT_FALSE(full_names.insert(FullName{.first_name = "Osi"}).second);
  EXPECT_TRUE(full_names.insert(FullName{.last_name = "Daro"}).second);
  EXPECT_FALSE(full_names.insert(FullName{.last_name = "Daro"}).second);
  EXPECT_TRUE(full_names.insert(FullName{}).second);
  EXPECT_FALSE(full_names.insert(FullName{}).second);
}

TEST(SoiagenTest, CreateWhole) {
  const CarOwner car_owner = CarOwner::whole{
      .car =
          Car::whole{
              .model = "Toyota",
              .owner = User{},
              .purchase_time = absl::FromUnixMillis(1000),
              .second_owner = absl::nullopt,
          },
      .owner =
          FullName::whole{
              .first_name = "Osi",
              .last_name = "Daro",
          },
  };
  EXPECT_EQ(car_owner,
            (CarOwner{.car =
                          {
                              .model = "Toyota",
                              .purchase_time = absl::FromUnixMillis(1000),
                          },
                      .owner = {
                          .first_name = "Osi",
                          .last_name = "Daro",
                      }}));
}

TEST(SoiagenTest, ReserializeStruct) {
  EXPECT_THAT(
      MakeReserializer(FullName{})
          .IsDefault()
          .ExpectDenseJson("[]")
          .ExpectReadableJson("{}")
          .ExpectBytes("f6")
          .ExpectDebugString("{}")
          .ExpectTypeDescriptorJson(
              "{\n  \"type\": {\n    \"kind\": \"record\",\n    \"value\": "
              "\"FullName:full_name.soia\"\n  },\n  \"records\": [\n    {\n    "
              "  \"kind\": \"STRUCT\",\n      \"id\": "
              "\"full_name.soia:FullName\",\n      \"fields\": [\n        {\n  "
              "        \"name\": \"first_name\",\n          \"type\": {\n      "
              "      \"kind\": \"primitive\",\n            \"value\": "
              "\"STRING\"\n          },\n          \"number\": 1\n        },\n "
              "       {\n          \"name\": \"last_name\",\n          "
              "\"type\": {\n            \"kind\": \"primitive\",\n            "
              "\"value\": \"STRING\"\n          },\n          \"number\": 4\n  "
              "      }\n      ],\n      \"removed_fields\": [\n        0,\n    "
              "    2,\n        3,\n        5\n      ]\n    }\n  ]\n}")
          .AddAlternativeBytes("00")
          .AddAlternativeJson("0")
          .Check(),
      IsOk());
  EXPECT_THAT(
      MakeReserializer(FullName{
                           .first_name = "Osi",
                           .last_name = "Daro",
                       })
          .ExpectDenseJson("[0,\"Osi\",0,0,\"Daro\"]")
          .ExpectReadableJson(
              "{\n  \"first_name\": \"Osi\",\n  \"last_name\": \"Daro\"\n}")
          .ExpectBytes("fa0500f3034f73690000f3044461726f")
          .ExpectDebugString(
              "{\n  .first_name: \"Osi\",\n  .last_name: \"Daro\",\n}")
          .Check(),
      IsOk());
  EXPECT_THAT(MakeReserializer(FullName{
                                   .first_name = "Osi",
                               })
                  .ExpectDenseJson("[0,\"Osi\"]")
                  .ExpectReadableJson("{\n  \"first_name\": \"Osi\"\n}")
                  .ExpectBytes("f800f3034f7369")
                  .ExpectDebugString("{\n  .first_name: \"Osi\",\n}")
                  .Check(),
              IsOk());
  EXPECT_THAT(MakeReserializer(FullName{
                                   .last_name = "Daro",
                               })
                  .ExpectDenseJson("[0,\"\",0,0,\"Daro\"]")
                  .ExpectReadableJson("{\n  \"last_name\": \"Daro\"\n}")
                  .ExpectBytes("fa0500f20000f3044461726f")
                  .ExpectDebugString("{\n  .last_name: \"Daro\",\n}")
                  .Check(),
              IsOk());
  EXPECT_THAT(
      MakeReserializer(CarOwner{
                           .car =
                               {
                                   .model = "Toyota",
                                   .purchase_time = absl::FromUnixMillis(1000),
                               },
                           .owner =
                               {
                                   .first_name = "Osi",
                                   .last_name = "Daro",
                               },
                       })
          .ExpectDenseJson("[[\"Toyota\",1000],[0,\"Osi\",0,0,\"Daro\"]]")
          .ExpectReadableJson(
              "{\n  \"car\": {\n    \"model\": \"Toyota\",\n    "
              "\"purchase_time\": {\n      \"unix_millis\": 1000,\n      "
              "\"formatted\": \"1970-01-01T00:00:01+00:00\"\n    }\n  },\n  "
              "\"owner\": {\n    \"first_name\": \"Osi\",\n    \"last_name\": "
              "\"Daro\"\n  }\n}")
          .ExpectBytes("f8f8f306546f796f7461efe803000000000000fa0500f3034f73690"
                       "000f3044461726f")
          .ExpectDebugString(
              "{\n  .car: {\n    .model: \"Toyota\",\n    .purchase_time: "
              "absl::FromUnixMillis(1000 /* 1970-01-01T00:00:01+00:00 */),\n  "
              "},\n  .owner: {\n    .first_name: \"Osi\",\n    .last_name: "
              "\"Daro\",\n  },\n}")
          .AddCompatibleSchema<Empty>("Empty")
          .Check(),
      IsOk());
  EXPECT_THAT(
      MakeReserializer(Bundle{
                           .f250 = true,
                       })
          .ExpectDenseJson(
              "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
              "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
              "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
              "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
              "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
              "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
              "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
              "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1]")
          .ExpectBytes(
              "fae8fb0000000000000000000000000000000000000000000000000000000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "00000000000000000000000000000000000000000000000000000000000001")
          .AddCompatibleSchema<Empty>("Empty")
          .AddCompatibleSchema<EmptyWithRm1>("EmptyWithRm1")
          .Check(),
      IsOk());
}

TEST(SoiagenTest, ParseStructFromInvalidJson) {
  EXPECT_THAT(soia::Parse<FullName>("{ \"first_name\": 1 }"), Not(IsOk()));
  EXPECT_THAT(soia::Parse<FullName>("{ \"first_name\": [ }"), Not(IsOk()));
  EXPECT_THAT(soia::Parse<FullName>("{ first_name: 0 "), Not(IsOk()));
}

TEST(SoiagenTest, StatusEnumSimpleOps) {
  StatusEnum();
  EXPECT_EQ(StatusEnum(), StatusEnum(soiagen::kUnknown));
  EXPECT_EQ(StatusEnum(soiagen::kUnknown), StatusEnum(soiagen::kUnknown));
  EXPECT_EQ(StatusEnum(soiagen::kUnknown), soiagen::kUnknown);
  EXPECT_EQ(soiagen::kUnknown, StatusEnum(soiagen::kUnknown));
  EXPECT_NE(StatusEnum(soiagen::kOk), StatusEnum(soiagen::kUnknown));
  EXPECT_NE(StatusEnum(soiagen::kOk), soiagen::kUnknown);
  EXPECT_EQ(soiagen::kOk, StatusEnum(soiagen::kOk));
  EXPECT_EQ(soiagen::kOk, StatusEnum(StatusEnum::kOk));
  EXPECT_EQ(StatusEnum(soiagen::kOk), soiagen::kOk);
  EXPECT_NE(soiagen::kOk, StatusEnum(soiagen::kUnknown));
  EXPECT_EQ(soiagen::kOk, StatusEnum(soiagen::kOk));
  EXPECT_NE(StatusEnum(soiagen::kOk), soiagen::kUnknown);
  EXPECT_NE(StatusEnum(soiagen::kOk), soiagen::kUnknown);
  StatusEnum e;
  EXPECT_EQ(e, soiagen::kUnknown);
  EXPECT_EQ(e.kind(), StatusEnum::kind_type::kConstUnknown);
  e = soiagen::kOk;
  EXPECT_EQ(e, soiagen::kOk);
  EXPECT_EQ(e.kind(), StatusEnum::kind_type::kConstOk);
  e = StatusEnum::wrap_error("E");
  EXPECT_EQ(e.kind(), StatusEnum::kind_type::kValError);
  ASSERT_TRUE(e.is_error());
  EXPECT_EQ(e.as_error(), "E");
  e.as_error() = "EE";
  EXPECT_EQ(e.as_error(), "EE");
  ASSERT_FALSE(StatusEnum().is_error());
}

TEST(SoiagenTest, StatusEnumEqAndHash) {
  absl::flat_hash_set<StatusEnum> statuses;
  EXPECT_TRUE(statuses.insert(soiagen::kUnknown).second);
  EXPECT_TRUE(statuses.insert(soiagen::kOk).second);
  EXPECT_FALSE(statuses.insert(soiagen::kOk).second);
  EXPECT_TRUE(statuses.insert(StatusEnum::wrap_error("E0")).second);
  EXPECT_TRUE(statuses.insert(StatusEnum::wrap_error("E1")).second);
  EXPECT_FALSE(statuses.insert(StatusEnum::wrap_error("E1")).second);
}

TEST(SoiagenTest, StatusEnumVisitReturnsRef) {
  struct Visitor {
    std::string a;
    std::string b;
    std::string c;

    std::string& operator()(soiagen::k_unknown) { return a; }
    std::string& operator()(soiagen::k_ok) { return b; }
    std::string& operator()(StatusEnum::wrap_error_type) { return c; }
  };
  static_assert(
      std::is_same_v<decltype(StatusEnum().visit(Visitor())), std::string&>);
  Visitor visitor;
  static_assert(
      std::is_same_v<decltype(StatusEnum(soiagen::kOk).visit(Visitor())),
                     std::string&>);
  visitor.b = "b";
  std::string& visit_result = StatusEnum(soiagen::kOk).visit(visitor);
  EXPECT_EQ(&visit_result, &visitor.b);
}

TEST(SoiagenTest, StatusEnumVisitExpectsConst) {
  struct Visitor {
    std::string e;

    void operator()(soiagen::k_unknown) {}
    void operator()(soiagen::k_ok) {}
    void operator()(const StatusEnum::wrap_error_type& error_wrapper) {
      e = error_wrapper.value;
    }
  };
  Visitor visitor;
  const StatusEnum status = StatusEnum::wrap_error("err");
  status.visit(visitor);
  EXPECT_EQ(visitor.e, "err");
}

TEST(SoiagenTest, StatusEnumVisitExpectsMutable) {
  struct Visitor {
    std::string e;

    void operator()(soiagen::k_unknown) {}
    void operator()(soiagen::k_ok) {}
    void operator()(StatusEnum::wrap_error_type& error_wrapper) {
      e.swap(error_wrapper.value);
    }
  };
  Visitor visitor;
  StatusEnum status = StatusEnum::wrap_error("err");
  status.visit(visitor);
  EXPECT_EQ(visitor.e, "err");
  EXPECT_EQ(status.is_error(), true);
  EXPECT_EQ(status.as_error(), "");
}

TEST(SoiagenTest, ReserializeEnum) {
  EXPECT_THAT(
      MakeReserializer(StatusEnum())
          .IsDefault()
          .ExpectDenseJson("0")
          .ExpectReadableJson("\"UNKNOWN\"")
          .ExpectDebugString("soiagen::kUnknown")
          .ExpectBytes("00")
          .ExpectTypeDescriptorJson(
              "{\n  \"type\": {\n    \"kind\": \"record\",\n    \"value\": "
              "\"Status:simple_enum.soia\"\n  },\n  \"records\": [\n    {\n    "
              "  \"kind\": \"ENUM\",\n      \"id\": "
              "\"simple_enum.soia:Status\",\n      \"fields\": [\n        {\n  "
              "        \"name\": \"UNKNOWN\"\n        },\n        {\n          "
              "\"name\": \"OK\",\n          \"number\": 1\n        },\n        "
              "{\n          \"name\": \"error\",\n          \"type\": {\n      "
              "      \"kind\": \"primitive\",\n            \"value\": "
              "\"STRING\"\n          },\n          \"number\": 2\n        }\n  "
              "    ]\n    }\n  ]\n}")
          .AddCompatibleSchema<EmptyEnum>("EmptyEnum")
          .Check(),
      IsOk());
  EXPECT_THAT(MakeReserializer(StatusEnum(StatusEnum::kOk))
                  .ExpectDenseJson("1")
                  .ExpectReadableJson("\"OK\"")
                  .ExpectDebugString("soiagen::kOk")
                  .ExpectBytes("01")
                  .AddCompatibleSchema<EmptyEnum>("EmptyEnum")
                  .Check(),
              IsOk());
  EXPECT_THAT(
      MakeReserializer(StatusEnum::wrap_error("E"))
          .ExpectDenseJson("[2,\"E\"]")
          .ExpectReadableJson(
              "{\n  \"kind\": \"error\",\n  \"value\": \"E\"\n}")
          .ExpectDebugString("::soiagen::wrap_error(\"E\")")
          .ExpectBytes("fcf30145")
          .AddAlternativeJson("{\"foo\":1,\"value\":\"E\",\"kind\":\"error\"}")
          .AddCompatibleSchema<EmptyEnum>("EmptyEnum")
          .Check(),
      IsOk());
  EXPECT_THAT(MakeReserializer(Weekday(Weekday::kUnknown)).IsDefault().Check(),
              IsOk());
  EXPECT_THAT(MakeReserializer(Weekday(Weekday::kMonday)).Check(), IsOk());
  EXPECT_THAT(MakeReserializer(JsonValue::wrap_boolean(true)).Check(), IsOk());
  EXPECT_THAT(
      MakeReserializer(EmptyEnum(EmptyEnum::kUnknown)).IsDefault().Check(),
      IsOk());
}

TEST(SoiagenTest, KeyedItems) {
  KeyedItems s;
  s.array_with_int32_key.push_back({
      .int32 = 10,
      .string = "foo 10",
  });
  s.array_with_int32_key.push_back({
      .int32 = 11,
      .string = "foo 11",
  });
  s.array_with_int32_key.push_back({
      .int32 = 12,
      .string = "foo 12",
  });
  s.array_with_int32_key.push_back({
      .int32 = 12,
      .string = "foo 12",
  });
  s.array_with_int32_key.push_back({
      .int32 = 13,
      .string = "foo 13",
  });
  EXPECT_EQ(s.array_with_int32_key.find_or_default(12), (Item{
                                                            .int32 = 12,
                                                            .string = "foo 12",
                                                        }));
  EXPECT_EQ(s.array_with_int32_key.find_or_default(14), Item{});
  EXPECT_EQ(s.array_with_int32_key.find_or_null(14), nullptr);
  EXPECT_THAT(
      MakeReserializer(s)
          .ExpectTypeDescriptorJson(
              "{\n  \"type\": {\n    \"kind\": \"record\",\n    \"value\": "
              "\"KeyedItems:structs.soia\"\n  },\n  \"records\": [\n    {\n    "
              "  \"kind\": \"STRUCT\",\n      \"id\": "
              "\"structs.soia:KeyedItems\",\n      \"fields\": [\n        {\n  "
              "        \"name\": \"array_with_bool_key\",\n          \"type\": "
              "{\n            \"kind\": \"array\",\n            \"value\": {\n "
              "             \"item\": {\n                \"kind\": "
              "\"record\",\n                \"value\": \"Item:structs.soia\"\n "
              "             },\n              \"key_chain\": [\n               "
              " \"bool\"\n              ]\n            }\n          }\n        "
              "},\n        {\n          \"name\": \"array_with_string_key\",\n "
              "         \"type\": {\n            \"kind\": \"array\",\n        "
              "    \"value\": {\n              \"item\": {\n                "
              "\"kind\": \"record\",\n                \"value\": "
              "\"Item:structs.soia\"\n              },\n              "
              "\"key_chain\": [\n                \"string\"\n              ]\n "
              "           }\n          },\n          \"number\": 1\n        "
              "},\n        {\n          \"name\": \"array_with_int32_key\",\n  "
              "        \"type\": {\n            \"kind\": \"array\",\n         "
              "   \"value\": {\n              \"item\": {\n                "
              "\"kind\": \"record\",\n                \"value\": "
              "\"Item:structs.soia\"\n              },\n              "
              "\"key_chain\": [\n                \"int32\"\n              ]\n  "
              "          }\n          },\n          \"number\": 2\n        "
              "},\n        {\n          \"name\": \"array_with_int64_key\",\n  "
              "        \"type\": {\n            \"kind\": \"array\",\n         "
              "   \"value\": {\n              \"item\": {\n                "
              "\"kind\": \"record\",\n                \"value\": "
              "\"Item:structs.soia\"\n              },\n              "
              "\"key_chain\": [\n                \"int64\"\n              ]\n  "
              "          }\n          },\n          \"number\": 3\n        "
              "},\n        {\n          \"name\": "
              "\"array_with_wrapper_key\",\n          \"type\": {\n            "
              "\"kind\": \"array\",\n            \"value\": {\n              "
              "\"item\": {\n                \"kind\": \"record\",\n            "
              "    \"value\": \"Item:structs.soia\"\n              },\n        "
              "      \"key_chain\": [\n                \"user\",\n             "
              "   \"id\"\n              ]\n            }\n          },\n       "
              "   \"number\": 4\n        },\n        {\n          \"name\": "
              "\"array_with_enum_key\",\n          \"type\": {\n            "
              "\"kind\": \"array\",\n            \"value\": {\n              "
              "\"item\": {\n                \"kind\": \"record\",\n            "
              "    \"value\": \"Item:structs.soia\"\n              },\n        "
              "      \"key_chain\": [\n                \"weekday\",\n          "
              "      \"kind\"\n              ]\n            }\n          },\n  "
              "        \"number\": 5\n        },\n        {\n          "
              "\"name\": \"array_with_bytes_key\",\n          \"type\": {\n    "
              "        \"kind\": \"array\",\n            \"value\": {\n        "
              "      \"item\": {\n                \"kind\": \"record\",\n      "
              "          \"value\": \"Item:structs.soia\"\n              },\n  "
              "            \"key_chain\": [\n                \"bytes\"\n       "
              "       ]\n            }\n          },\n          \"number\": "
              "6\n        },\n        {\n          \"name\": "
              "\"array_with_timestamp_key\",\n          \"type\": {\n          "
              "  \"kind\": \"array\",\n            \"value\": {\n              "
              "\"item\": {\n                \"kind\": \"record\",\n            "
              "    \"value\": \"Item:structs.soia\"\n              },\n        "
              "      \"key_chain\": [\n                \"timestamp\"\n         "
              "     ]\n            }\n          },\n          \"number\": 7\n  "
              "      }\n      ]\n    },\n    {\n      \"kind\": \"STRUCT\",\n  "
              "    \"id\": \"structs.soia:Item\",\n      \"fields\": [\n       "
              " {\n          \"name\": \"bool\",\n          \"type\": {\n      "
              "      \"kind\": \"primitive\",\n            \"value\": "
              "\"BOOL\"\n          }\n        },\n        {\n          "
              "\"name\": \"string\",\n          \"type\": {\n            "
              "\"kind\": \"primitive\",\n            \"value\": \"STRING\"\n   "
              "       },\n          \"number\": 1\n        },\n        {\n     "
              "     \"name\": \"int32\",\n          \"type\": {\n            "
              "\"kind\": \"primitive\",\n            \"value\": \"INT32\"\n    "
              "      },\n          \"number\": 2\n        },\n        {\n      "
              "    \"name\": \"int64\",\n          \"type\": {\n            "
              "\"kind\": \"primitive\",\n            \"value\": \"INT64\"\n    "
              "      },\n          \"number\": 3\n        },\n        {\n      "
              "    \"name\": \"user\",\n          \"type\": {\n            "
              "\"kind\": \"record\",\n            \"value\": "
              "\"User:structs.soia\"\n          },\n          \"number\": 4\n  "
              "      },\n        {\n          \"name\": \"weekday\",\n         "
              " \"type\": {\n            \"kind\": \"record\",\n            "
              "\"value\": \"Weekday:enums.soia\"\n          },\n          "
              "\"number\": 5\n        },\n        {\n          \"name\": "
              "\"bytes\",\n          \"type\": {\n            \"kind\": "
              "\"primitive\",\n            \"value\": \"STRING\"\n          "
              "},\n          \"number\": 6\n        },\n        {\n          "
              "\"name\": \"timestamp\",\n          \"type\": {\n            "
              "\"kind\": \"primitive\",\n            \"value\": "
              "\"TIMESTAMP\"\n          },\n          \"number\": 7\n        "
              "}\n      ]\n    },\n    {\n      \"kind\": \"STRUCT\",\n      "
              "\"id\": \"structs.soia:User\",\n      \"fields\": [\n        "
              "{\n          \"name\": \"id\",\n          \"type\": {\n         "
              "   \"kind\": \"primitive\",\n            \"value\": "
              "\"STRING\"\n          }\n        }\n      ]\n    },\n    {\n    "
              "  \"kind\": \"ENUM\",\n      \"id\": \"enums.soia:Weekday\",\n  "
              "    \"fields\": [\n        {\n          \"name\": \"UNKNOWN\"\n "
              "       },\n        {\n          \"name\": \"MONDAY\",\n         "
              " \"number\": 1\n        },\n        {\n          \"name\": "
              "\"TUESDAY\",\n          \"number\": 2\n        },\n        {\n  "
              "        \"name\": \"WEDNESDAY\",\n          \"number\": 3\n     "
              "   },\n        {\n          \"name\": \"THURSDAY\",\n          "
              "\"number\": 4\n        },\n        {\n          \"name\": "
              "\"FRIDAY\",\n          \"number\": 5\n        },\n        {\n   "
              "       \"name\": \"SATURDAY\",\n          \"number\": 6\n       "
              " },\n        {\n          \"name\": \"SUNDAY\",\n          "
              "\"number\": 7\n        }\n      ]\n    }\n  ]\n}")
          .Check(),
      IsOk());
}

TEST(SoiagenTest, Constants) {
  soiagen_constants::k_one_constant();
  EXPECT_EQ(soiagen_constants::k_one_single_quoted_string(),
            std::string("\"\0Pokémon\n\"", 12));
}

TEST(SoiagenTest, Methods) {
  using ::soiagen_methods::MyProcedure;
  static_assert(std::is_same_v<typename MyProcedure::input_type,
                               soiagen_structs::Point>);
  static_assert(std::is_same_v<typename MyProcedure::output_type,
                               soiagen_enums::JsonValue>);
  constexpr int kNumber = MyProcedure::kNumber;
  EXPECT_EQ(kNumber, 1974132327);
  constexpr absl::string_view kMethodName = MyProcedure::kMethodName;
  EXPECT_EQ(kMethodName, "MyProcedure");
}

TEST(SoiagenTest, NestedRecordAlias) {
  static_assert(
      std::is_same_v<soiagen_structs::Item::User, soiagen_structs::Item_User>);
  static_assert(
      std::is_same_v<soiagen_structs::Name::Name_, soiagen_structs::Name_Name>);
  static_assert(std::is_same_v<soiagen_structs::Name::Name_::Name,
                               soiagen_structs::Name_Name_Name>);
  static_assert(std::is_same_v<soiagen_enums::JsonValue::Pair,
                               soiagen_enums::JsonValue_Pair>);
}

TEST(SoiagenTest, StructMatcher) {
  const FullName full_name = {
      .first_name = "John",
      .last_name = "Doe",
  };
  EXPECT_THAT(full_name, (::testing::soiagen::StructIs<FullName>{
                             .first_name = testing::StartsWith("J"),
                         }));

  const CarOwner car_owner = CarOwner::whole{
      .car =
          Car::whole{
              .model = "Toyota",
              .owner = User{},
              .purchase_time = absl::FromUnixMillis(1000),
              .second_owner = absl::nullopt,
          },
      .owner =
          FullName::whole{
              .first_name = "Osi",
              .last_name = "Daro",
          },
  };
  EXPECT_THAT(car_owner, (::testing::soiagen::StructIs<CarOwner>{
                             .car =
                                 {
                                     .model = testing::StartsWith("To"),
                                     .purchase_time = testing::_,
                                     .second_owner = testing::Eq(absl::nullopt),
                                 },
                             .owner = {
                                 .first_name = "Osi",
                             }}));

  EXPECT_THAT((soiagen_enums::WeekdayHolder{
                  .weekday = soiagen_enums::Weekday::kFriday,
              }),
              (::testing::soiagen::StructIs<soiagen_enums::WeekdayHolder>{
                  .weekday = testing::Eq(soiagen_enums::Weekday::kFriday),
              }));
}

TEST(SoiagenTest, EnumValueMatcher) {
  EXPECT_THAT(StatusEnum::wrap_error("E"), ::testing::soiagen::IsError());
  EXPECT_THAT(StatusEnum::wrap_error("E"),
              ::testing::soiagen::IsError(::testing::StartsWith("E")));
}

struct FieldNameCollector {
  absl::flat_hash_set<absl::string_view> field_names;

  template <typename Getter, typename Value>
  void operator()(soia::reflection::struct_field<Getter, Value>) {
    field_names.insert(Getter::kFieldName);
  }

  template <typename Const>
  void operator()(soia::reflection::enum_const_field<Const>) {
    field_names.insert(Const::kFieldName);
  }

  template <typename Option, typename Value>
  void operator()(soia::reflection::enum_value_field<Option, Value>) {
    field_names.insert(Option::kFieldName);
  }
};

TEST(SoiagenTest, ForEachFieldOfEnum) {
  FieldNameCollector collector;
  soia::reflection::ForEachField<StatusEnum>(collector);
  EXPECT_THAT(collector.field_names,
              UnorderedElementsAre("OK", "UNKNOWN", "error"));
}

TEST(SoiagenTest, ForEachFieldOfStruct) {
  FieldNameCollector collector;
  soia::reflection::ForEachField<FullName>(collector);
  EXPECT_THAT(collector.field_names,
              UnorderedElementsAre("first_name", "last_name"));
}

TEST(SoialibTest, SoiaApi) {
  class FakeApiImpl {
   public:
    using methods_type = std::tuple<soiagen_methods::MyProcedure,
                                    soiagen_methods::WithExplicitNumber>;

    ::soiagen_enums::JsonValue operator()(soiagen_methods::MyProcedure,
                                          const ::soiagen_structs::Point& input,
                                          int& status_code) {
      return ::soiagen_enums::JsonValue::wrap_number(input.x);
    }

    absl::StatusOr<::absl::optional<::soiagen_enums::JsonValue>> operator()(
        soiagen_methods::WithExplicitNumber,
        const std::vector<::soiagen_structs::Point>& input,
        int& status_code) const {
      if (input.empty()) {
        return absl::UnknownError("no point");
      }
      return absl::nullopt;
    }
  };

  FakeApiImpl api_impl;
  std::unique_ptr<soia::api::ApiClient> api_client =
      soia::api::MakeApiClientForTesting<FakeApiImpl>(&api_impl);

  {
    const absl::StatusOr<::soiagen_enums::JsonValue> result =
        ::soia::api::InvokeRemote(*api_client, soiagen_methods::MyProcedure(),
                                  soiagen_structs::Point{.x = 1, .y = 2});
    EXPECT_THAT(result,
                IsOkAndHolds(::soiagen_enums::JsonValue::wrap_number(1.0)));
  }

  {
    const absl::StatusOr<::absl::optional<::soiagen_enums::JsonValue>> result =
        ::soia::api::InvokeRemote(*api_client,
                                  soiagen_methods::WithExplicitNumber(), {});
    EXPECT_EQ(result.status(), absl::UnknownError("no point"));
  }

  {
    const absl::StatusOr<std::string> result =
        ::soia::api::InvokeRemote(*api_client, soiagen_methods::True(), "foo");
    EXPECT_EQ(result.status(),
              absl::UnknownError("Method not found: True; number: 2615726"));
  }
}

}  // namespace
