#include <gtest/gtest.h>

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
using ::soia::api::HttpHeaders;
using ::soia::api::InvokeRemote;
using ::soia::api::MakeHttplibApiClient;
using ::soia::api::MountApiToHttplibServer;
using ::soiagen_methods::ListUsers;
using ::soiagen_methods::ListUsersRequest;
using ::soiagen_methods::ListUsersResponse;
using ::soiagen_methods::User;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Pair;
using ::testing::soiagen::StructIs;

class ApiImpl {
 public:
  using methods = std::tuple<soiagen_methods::ListUsers>;

  absl::StatusOr<ListUsersResponse> operator()(
      ListUsers, const ListUsersRequest& request,
      const soia::api::HttpHeaders& request_headers,
      soia::api::HttpHeaders& response_headers) const {
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

TEST(SoiaApiTest, TestServerAndClientWithMetadata) {
  constexpr int kPort = 8787;

  httplib::Server server;

  auto api_impl = std::make_shared<ApiImpl>();
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
  std::unique_ptr<soia::api::ApiClient> api_client =
      MakeHttplibApiClient(&client, "/myapi");

  HttpHeaders request_headers;
  request_headers.Insert("foo", "bar");
  HttpHeaders response_headers;
  response_headers.Insert("zoo", "rab");
  absl::StatusOr<ListUsersResponse> response =
      InvokeRemote(*api_client, ListUsers(), ListUsersRequest{.country = "AU"},
                   request_headers, &response_headers);

  EXPECT_THAT(response, IsOkAndHolds(StructIs<ListUsersResponse>{
                            .users = ElementsAre(StructIs<User>{
                                .first_name = "Jane",
                            })}));

  EXPECT_THAT(response_headers.map(),
              Contains(Pair("foo", ElementsAre("bar"))));

  EXPECT_THAT(InvokeRemote(*api_client, ListUsers(), ListUsersRequest{},
                           request_headers, &response_headers)
                  .status(),
              absl::UnknownError("Server error: no country specified"));

  EXPECT_THAT(response_headers.map(),
              Contains(Pair("x-foo", ElementsAre("bar"))));

  EXPECT_THAT(
      InvokeRemote(*api_client, soiagen_methods::True(), "", {}).status(),
      absl::UnknownError(
          "Bad request: Method not found: True; number: 2615726"));

  server.stop();
  server_thread.join();
}

}  // namespace
