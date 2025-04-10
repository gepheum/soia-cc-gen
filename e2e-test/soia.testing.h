// Soia client library for testing

#ifndef SOIA_SOIA_TESTING_H
#define SOIA_SOIA_TESTING_H

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/log/die_if_null.h"
#include "gmock/gmock.h"
#include "soia.h"

namespace testing {
namespace soiagen {
template <typename Struct>
struct StructIs {
  static_assert(soia::reflection::IsStruct<Struct>());
  static_assert(false, "did you forget to include the *.testing.h file?");
};
}  // namespace soiagen

namespace soia_internal {
template <typename Matcher>
bool IsAnythingMatcher(const Matcher& matcher) {
  static const std::string* const kIsAnything = []() {
    std::stringstream ss;
    _.DescribeTo(&ss);
    return new std::string(ss.str());
  }();
  std::stringstream ss;
  matcher.DescribeTo(&ss);
  return ss.str() == *kIsAnything;
}

namespace field_matcher {

// Helper function to do comma folding in C++11.
// The array ensures left-to-right order of evaluation.
// Usage: VariadicExpand({expr...});
template <typename T, size_t N>
void VariadicExpand(const T (&)[N]) {}

template <typename Getter, typename Matcher>
std::nullopt_t DescribeTo(const std::pair<Getter, Matcher>& field_matcher,
                          ::std::ostream* os, bool& first) {
  const Matcher& matcher = field_matcher.second;
  if (IsAnythingMatcher(matcher)) return std::nullopt;
  *os << (first ? "" : ", and ") << "has field " << Getter::kFieldName
      << " that ";
  matcher.DescribeTo(os);
  first = false;
  return std::nullopt;
}

template <typename Getter, typename Matcher>
std::nullopt_t DescribeNegationTo(
    const std::pair<Getter, Matcher>& field_matcher, ::std::ostream* os,
    bool& first) {
  const Matcher& matcher = field_matcher.second;

  if (IsAnythingMatcher(matcher)) return std::nullopt;
  *os << (first ? "" : ", or ") << "has field " << Getter::kFieldName
      << " that ";
  matcher.DescribeNegationTo(os);
  first = false;
  return std::nullopt;
}

template <size_t I, typename Struct, typename FieldMatcher>
std::nullopt_t MatchAndExplain(const FieldMatcher& field_matcher,
                               const Struct& t,
                               StringMatchResultListener* inner_listener,
                               size_t& failed_pos) {
  if (failed_pos != ~size_t{}) return std::nullopt;
  const auto& [getter, matcher] = field_matcher;
  if (IsAnythingMatcher(matcher)) return std::nullopt;
  if (!matcher.MatchAndExplain(getter(t), inner_listener)) {
    failed_pos = I;
  }
  return std::nullopt;
}

template <typename FieldMatcher, typename Struct>
bool Matches(const FieldMatcher& field_matcher, const Struct& t) {
  const auto& [getter, matcher] = field_matcher;
  if (IsAnythingMatcher(matcher)) return true;
  return matcher.Matches(getter(t));
}

}  // namespace field_matcher

template <typename Struct, typename FieldMatchers, typename StructSize>
class StructIsMatcherImpl;

template <typename Struct, typename FieldMatchers, size_t... I>
class StructIsMatcherImpl<Struct, FieldMatchers, std::index_sequence<I...>>
    : public MatcherInterface<const Struct&> {
 public:
  explicit StructIsMatcherImpl(FieldMatchers matchers)
      : matchers_(std::move(matchers)) {
    field_matcher::VariadicExpand({(field_names_.emplace_back(
        decltype(std::get<I>(matchers_).first)::kFieldName))...});
  }

  void DescribeTo(::std::ostream* os) const override {
    bool first = true;
    field_matcher::VariadicExpand(
        {(field_matcher::DescribeTo(std::get<I>(matchers_), os, first))...});
    if (first) {
      *os << "is anything";
    }
  }

  void DescribeNegationTo(::std::ostream* os) const override {
    bool first = true;
    field_matcher::VariadicExpand({(field_matcher::DescribeNegationTo(
        std::get<I>(matchers_), os, first))...});
    if (first) {
      *os << "never matches";
    }
  }

  bool MatchAndExplain(const Struct& t,
                       MatchResultListener* listener) const override {
    return MatchInternal(t, listener);
  }

 private:
  bool MatchInternal(const Struct& t, MatchResultListener* listener) const {
    if (!listener->IsInterested()) {
      // If the listener is not interested, we don't need to construct the
      // explanation.
      bool good = true;
      field_matcher::VariadicExpand(
          {good =
               good && field_matcher::Matches(std::get<I>(matchers_), t)...});
      return good;
    }
    size_t failed_pos = ~size_t{};
    std::vector<StringMatchResultListener> inner_listener(sizeof...(I));
    field_matcher::VariadicExpand({field_matcher::MatchAndExplain<I>(
        std::get<I>(matchers_), t, &inner_listener[I], failed_pos)...});
    if (failed_pos != ~size_t{}) {
      *listener << "whose field " << field_names_[failed_pos]
                << " does not match";
      const std::string explanation = inner_listener[failed_pos].str();
      if (!explanation.empty() && listener->stream() != nullptr) {
        *listener->stream() << ", " << explanation;
      }
      return false;
    }
    *listener << "whose all elements match";
    const char* separator = ", where";
    for (size_t index = 0; index < sizeof...(I); ++index) {
      const std::string str = inner_listener[index].str();
      if (!str.empty()) {
        *listener << separator << " field " << field_names_[index]
                  << " is a value " << str;
        separator = ", and";
      }
    }
    return true;
  }

  FieldMatchers matchers_;
  std::vector<absl::string_view> field_names_;
};

template <typename Struct, typename... FieldMatcher>
Matcher<Struct> StructIs(FieldMatcher... matchers) {
  return Matcher<Struct>(
      new StructIsMatcherImpl<Struct, tuple<FieldMatcher...>,
                              std::index_sequence_for<FieldMatcher...>>(
          std::make_tuple(matchers...)));
}

template <typename Option, typename InnerMatcher>
class EnumValueIsMatcher {
 public:
  explicit EnumValueIsMatcher(InnerMatcher matcher)
      : matcher_(std::move(matcher)) {}

  template <typename Enum>
  operator Matcher<Enum>() const {
    return Matcher<Enum>(new Impl<Enum>(matcher_));
  }

 private:
  // The monomorphic implementation that works for a particular enum type.
  template <typename Enum>
  class Impl : public MatcherInterface<const Enum&> {
   public:
    using value_type =
        decltype(*Option::get_or_null(std::declval<const Enum&>()));

    explicit Impl(const InnerMatcher& matcher)
        : matcher_(MatcherCast<const value_type&>(matcher)) {}

    void DescribeTo(::std::ostream* os) const override {
      *os << "is a " << Option::kFieldName;
      if (!IsAnythingMatcher(matcher_)) {
        *os << " that ";
        matcher_.DescribeTo(os);
      }
    }

    void DescribeNegationTo(::std::ostream* os) const override {
      *os << "is not a " << Option::kFieldName;
      if (!IsAnythingMatcher(matcher_)) {
        *os << " that ";
        matcher_.DescribeTo(os);
      }
    }

    bool MatchAndExplain(const Enum& e,
                         MatchResultListener* listener) const override {
      const auto* actual_value = Option::get_or_null(e);
      if (actual_value == nullptr) {
        *listener << "which is " << e;
        return false;
      } else {
        *listener << "which is ";
        return MatchPrintAndExplain(*actual_value, matcher_, listener);
      }
    }

   private:
    const Matcher<value_type> matcher_;
  };

  const InnerMatcher matcher_;
};

template <typename Option, typename InnerMatcher>
EnumValueIsMatcher<Option, InnerMatcher> EnumValueIs(InnerMatcher matcher) {
  return EnumValueIsMatcher<Option, InnerMatcher>(std::move(matcher));
}

template <typename FakeOrMockApiImpl>
class ClientForTesting : public ::soia::service::Client {
 public:
  explicit ClientForTesting(FakeOrMockApiImpl* api_impl)
      : api_impl_(*ABSL_DIE_IF_NULL(api_impl)) {}

  virtual ~ClientForTesting() = default;

  absl::StatusOr<std::string> operator()(
      absl::string_view request_data,
      const soia::service::HttpHeaders& request_headers,
      soia::service::HttpHeaders& response_headers) const override {
    return ::soia::service::HandleRequest(
               api_impl_, request_data,
               [](absl::string_view) { return std::string(); }, request_headers,
               response_headers)
        .AsStatus();
  }

 private:
  FakeOrMockApiImpl& api_impl_;
};

}  // namespace soia_internal
}  // namespace testing

namespace soia {
namespace service {

template <typename FakeOrMockApiImpl>
std::unique_ptr<::soia::service::Client> MakeClientForTesting(
    absl::Nonnull<FakeOrMockApiImpl*> api_impl) {
  return std::make_unique<
      testing::soia_internal::ClientForTesting<FakeOrMockApiImpl>>(api_impl);
}

}  // namespace service
}  // namespace soia

#endif
