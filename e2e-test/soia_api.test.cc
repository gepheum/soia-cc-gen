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
#include "gmock/gmock.h"
#include "httplib.h"
#include "soia.h"
#include "soiagen/methods.h"
#include "soiagen/methods.testing.h"

namespace {
using ::absl_testing::IsOkAndHolds;
using ::soia::api::HttpMeta;
using ::soia::api::HttpRequestMeta;
using ::soia::api::HttpResponseMeta;
using ::soia::api::InvokeRemote;
using ::soia::api::MakeHttplibApiClient;
using ::soia::api::MountApiToHttplibServer;
using ::soia::api::NoMeta;
using ::soiagen_methods::ListUsers;
using ::soiagen_methods::ListUsersRequest;
using ::soiagen_methods::ListUsersResponse;
using ::soiagen_methods::User;
using ::testing::_;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Not;
using ::testing::Pair;
using ::testing::soiagen::StructIs;

class ApiImplNoMeta {
 public:
  using meta = NoMeta;

  using methods = std::tuple<soiagen_methods::ListUsers>;

  absl::StatusOr<ListUsersResponse> operator()(
      ListUsers, const ListUsersRequest& request) const {
    if (request.country.empty()) {
      return absl::UnknownError("no country specified");
    }
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

TEST(SoiaApiTest, TestServerAndClientWithNoMetadata) {
  constexpr int kPort = 8787;

  httplib::Server server;

  auto api_impl = std::make_shared<ApiImplNoMeta>();
  MountApiToHttplibServer(server, "/myapi", api_impl);

  api_impl->AddUser({
      .id = 100,
      .first_name = "John",
      .last_name = "Doe",
      .country = "US",
  });
  api_impl->AddUser({
      .id = 101,
      .first_name = "James",
      .last_name = "Doe",
      .country = "US",
  });
  api_impl->AddUser({
      .id = 102,
      .first_name = "Jane",
      .last_name = "Doe",
      .country = "AU",
  });

  std::thread server_thread([&server]() { server.listen("localhost", kPort); });

  server.wait_until_ready();

  httplib::Client client("localhost", kPort);
  std::unique_ptr<soia::api::ApiClient<>> api_client =
      MakeHttplibApiClient(&client, "/myapi");

  absl::StatusOr<ListUsersResponse> response =
      InvokeRemote(*api_client, ListUsers(), ListUsersRequest{.country = "AU"});

  EXPECT_THAT(response, IsOkAndHolds(StructIs<ListUsersResponse>{
                            .users = ElementsAre(StructIs<User>{
                                .first_name = "Jane",
                            })}));

  EXPECT_THAT(
      InvokeRemote(*api_client, ListUsers(), ListUsersRequest{}).status(),
      absl::UnknownError("Server error: no country specified"));

  EXPECT_THAT(InvokeRemote(*api_client, soiagen_methods::True(), "").status(),
              absl::UnknownError(
                  "Bad request: Method not found: True; number: 2615726"));

  server.stop();
  server_thread.join();
}

class ApiImplWithMeta {
 public:
  using meta = HttpMeta;

  using methods = std::tuple<soiagen_methods::ListUsers>;

  absl::StatusOr<ListUsersResponse> operator()(
      ListUsers, const ListUsersRequest& request,
      const HttpRequestMeta& request_meta,
      HttpResponseMeta& response_meta) const {
    if (request.country.empty()) {
      response_meta.status = 501;
      return absl::UnknownError("no country specified");
    }
    response_meta.status = 201;
    response_meta.headers = request_meta.headers;
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

TEST(SoiaApiTest, TestServerAndClientWithMetadata) {
  constexpr int kPort = 8787;

  httplib::Server server;

  auto api_impl = std::make_shared<ApiImplWithMeta>();
  MountApiToHttplibServer(server, "/myapi", api_impl);

  api_impl->AddUser({
      .id = 102,
      .first_name = "Jane",
      .last_name = "Doe",
      .country = "AU",
  });

  std::thread server_thread([&server]() { server.listen("localhost", kPort); });

  server.wait_until_ready();

  httplib::Client client("localhost", kPort);
  std::unique_ptr<soia::api::ApiClient<HttpMeta>> api_client =
      MakeHttplibApiClient(&client, "/myapi", HttpMeta());

  HttpRequestMeta request_meta;
  request_meta.headers.Add("foo", "bar");
  HttpResponseMeta response_meta;
  response_meta.headers.Add("zoo", "rab");
  absl::StatusOr<ListUsersResponse> response =
      InvokeRemote(*api_client, ListUsers(), ListUsersRequest{.country = "AU"},
                   request_meta, &response_meta);

  EXPECT_THAT(response, IsOkAndHolds(StructIs<ListUsersResponse>{
                            .users = ElementsAre(StructIs<User>{
                                .first_name = "Jane",
                            })}));

  EXPECT_EQ(response_meta.status, 201);
  EXPECT_THAT(response_meta.headers.map(),
              testing::Contains(Pair("foo", ElementsAre("bar"))));

  EXPECT_THAT(InvokeRemote(*api_client, ListUsers(), ListUsersRequest{},
                           request_meta, &response_meta)
                  .status(),
              absl::UnknownError("Server error: no country specified"));

  EXPECT_EQ(response_meta.status, 501);
  EXPECT_THAT(response_meta.headers.map(), Not(Contains(Pair("foo", _))));

  server.stop();
  server_thread.join();
}

}  // namespace
