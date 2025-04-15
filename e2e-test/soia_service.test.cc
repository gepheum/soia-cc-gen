#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "httplib.h"
#include "soia.h"
#include "soiagen/methods.h"
#include "soiagen/methods.testing.h"

namespace {
using ::absl_testing::IsOkAndHolds;
using ::soia::service::HttpHeaders;
using ::soia::service::InstallServiceOnHttplibServer;
using ::soia::service::InvokeRemote;
using ::soia::service::MakeHttplibClient;
using ::soiagen_methods::ListUsers;
using ::soiagen_methods::ListUsersRequest;
using ::soiagen_methods::ListUsersResponse;
using ::soiagen_methods::User;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Pair;
using ::testing::soiagen::StructIs;

class ServiceImpl {
 public:
  using methods = std::tuple<soiagen_methods::ListUsers>;

  absl::StatusOr<ListUsersResponse> operator()(
      ListUsers, ListUsersRequest request,
      const soia::service::HttpHeaders& request_headers,
      soia::service::HttpHeaders& response_headers) const {
    if (request.country.empty()) {
      response_headers.Insert("X-foo", "bar");
      return absl::UnknownError("no country specified");
    }
    response_headers = request_headers;
    ListUsersResponse response;
    auto it = country_to_users.find(request.country);
    if (it != country_to_users.end()) {
      for (const User& user : it->second) {
        response.users.push_back(user);
      }
    }
    return response;
  }

  void AddUser(User user) {
    country_to_users[user.country].push_back(std::move(user));
  }

 private:
  absl::flat_hash_map<std::string, std::vector<User>> country_to_users;
};

class ServiceImplNoMethod {
 public:
  using methods = std::tuple<>;
};

TEST(SoiaServiceTest, TestServerAndClient) {
  constexpr int kPort = 8787;

  httplib::Server server;

  auto service_impl = std::make_shared<ServiceImpl>();
  InstallServiceOnHttplibServer(server, "/myapi", service_impl);

  service_impl->AddUser({
      .id = 102,
      .first_name = "Jane",
      .last_name = "Doe",
      .country = "AU",
  });

  std::thread server_thread([&server]() { server.listen("localhost", kPort); });

  server.wait_until_ready();

  httplib::Client client("localhost", kPort);
  std::unique_ptr<soia::service::Client> soia_client =
      MakeHttplibClient(&client, "/myapi");

  HttpHeaders request_headers;
  request_headers.Insert("foo", "bar");
  HttpHeaders response_headers;
  response_headers.Insert("zoo", "rab");
  absl::StatusOr<ListUsersResponse> response =
      InvokeRemote(*soia_client, ListUsers(), ListUsersRequest{.country = "AU"},
                   request_headers, &response_headers);

  EXPECT_THAT(response, IsOkAndHolds(StructIs<ListUsersResponse>{
                            .users = ElementsAre(StructIs<User>{
                                .first_name = "Jane",
                            })}));

  EXPECT_THAT(response_headers.map(),
              Contains(Pair("foo", ElementsAre("bar"))));

  EXPECT_THAT(
      InvokeRemote(*soia_client, ListUsers(), ListUsersRequest{},
                   request_headers, &response_headers)
          .status(),
      absl::UnknownError(
          "HTTP response status 500: server error: no country specified"));

  EXPECT_THAT(response_headers.map(),
              Contains(Pair("x-foo", ElementsAre("bar"))));

  EXPECT_THAT(
      InvokeRemote(*soia_client, soiagen_methods::True(), "", {}).status(),
      absl::UnknownError("HTTP response status 400: bad request: method not "
                         "found: True; number: 2615726"));

  // Send GET requests.

  {
    auto result = client.Get(
        absl::StrCat("/myapi?foo:", ListUsers::kNumber, "%3Areadable:{%7d"));
    EXPECT_TRUE(result);
    EXPECT_EQ(result->status, 500);
    EXPECT_EQ(result->body, "server error: no country specified");
  }

  {
    auto result = client.Get(absl::StrCat("/myapi?foo:", ListUsers::kNumber,
                                          "%3Areadable:[\"AU\"]"));
    EXPECT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->body,
              "{\n  \"users\": [\n    {\n      \"id\": 102,\n      "
              "\"first_name\": \"Jane\",\n      \"last_name\": \"Doe\",\n      "
              "\"country\": \"AU\"\n    }\n  ]\n}");
  }

  {
    auto result = client.Get("/myapi?list");
    EXPECT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(
        result->body,
        "{\n  \"methods\": [\n    {\n      \"method\": \"ListUsers\",\n      "
        "\"number\": 770621418,\n      \"request\": {\n        \"type\": {\n   "
        "       \"kind\": \"record\",\n          \"value\": "
        "\"methods.soia:ListUsersRequest\"\n        },\n        \"records\": "
        "[\n          {\n            \"kind\": \"struct\",\n            "
        "\"id\": \"methods.soia:ListUsersRequest\",\n            \"fields\": "
        "[\n              {\n                \"name\": \"country\",\n          "
        "      \"type\": {\n                  \"kind\": \"primitive\",\n       "
        "           \"value\": \"string\"\n                }\n              "
        "}\n            ]\n          }\n        ]\n      },\n      "
        "\"response\": {\n        \"type\": {\n          \"kind\": "
        "\"record\",\n          \"value\": "
        "\"methods.soia:ListUsersResponse\"\n        },\n        \"records\": "
        "[\n          {\n            \"kind\": \"struct\",\n            "
        "\"id\": \"methods.soia:ListUsersResponse\",\n            \"fields\": "
        "[\n              {\n                \"name\": \"users\",\n            "
        "    \"type\": {\n                  \"kind\": \"array\",\n             "
        "     \"value\": {\n                    \"item\": {\n                  "
        "    \"kind\": \"record\",\n                      \"value\": "
        "\"methods.soia:User\"\n                    },\n                    "
        "\"key_chain\": [\n                      \"id\"\n                    "
        "]\n                  }\n                }\n              }\n          "
        "  ]\n          },\n          {\n            \"kind\": \"struct\",\n   "
        "         \"id\": \"methods.soia:User\",\n            \"fields\": [\n  "
        "            {\n                \"name\": \"id\",\n                "
        "\"type\": {\n                  \"kind\": \"primitive\",\n             "
        "     \"value\": \"uint64\"\n                }\n              },\n     "
        "         {\n                \"name\": \"first_name\",\n               "
        " \"type\": {\n                  \"kind\": \"primitive\",\n            "
        "      \"value\": \"string\"\n                },\n                "
        "\"number\": 1\n              },\n              {\n                "
        "\"name\": \"last_name\",\n                \"type\": {\n               "
        "   \"kind\": \"primitive\",\n                  \"value\": "
        "\"string\"\n                },\n                \"number\": 2\n       "
        "       },\n              {\n                \"name\": \"country\",\n  "
        "              \"type\": {\n                  \"kind\": "
        "\"primitive\",\n                  \"value\": \"string\"\n             "
        "   },\n                \"number\": 3\n              }\n            "
        "]\n          }\n        ]\n      }\n    }\n  ]\n}");
  }

  server.stop();
  server_thread.join();
}

TEST(SoiaServiceTest, NoMethod) {
  constexpr int kPort = 8787;

  httplib::Server server;

  auto service_impl = std::make_shared<ServiceImplNoMethod>();
  InstallServiceOnHttplibServer(server, "/myapi", service_impl);

  std::thread server_thread([&server]() { server.listen("localhost", kPort); });

  server.wait_until_ready();

  httplib::Client client("localhost", kPort);

  auto result = client.Get("/myapi?list");
  EXPECT_TRUE(result);
  EXPECT_EQ(result->status, 200);
  EXPECT_EQ(result->body, "{\n  \"methods\": []\n}");

  server.stop();
  server_thread.join();
}

}  // namespace
