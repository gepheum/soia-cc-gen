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

TEST(SoiaServiceTest, TestServerAndClientWithMetadata) {
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

  std::unique_ptr<soia::service::Client> soia_client = MakeHttplibClient(
      std::make_unique<httplib::Client>("localhost", kPort), "/myapi");

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

  server.stop();
  server_thread.join();
}

}  // namespace
