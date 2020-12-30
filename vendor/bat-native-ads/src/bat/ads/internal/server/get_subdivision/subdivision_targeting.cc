/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ads/internal/server/get_subdivision/subdivision_targeting.h"

#include <stdint.h>

#include <functional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/stringprintf.h"
#include "brave/components/l10n/browser/locale_helper.h"
#include "brave/components/l10n/common/locale_util.h"
#include "bat/ads/ads_client.h"
#include "bat/ads/internal/ads_impl.h"
#include "bat/ads/internal/logging.h"
#include "bat/ads/internal/locale/supported_subdivision_codes.h"
#include "bat/ads/internal/server/ads_server_util.h"
#include "bat/ads/internal/server/get_subdivision/get_subdivision_url_request_builder.h"
#include "bat/ads/internal/time_util.h"
#include "bat/ads/pref_names.h"

namespace ads {

using std::placeholders::_1;

namespace {

const uint64_t kRetryAfterSeconds = 1 * base::Time::kSecondsPerMinute;

const uint64_t kFetchSubdivisionTargetingPing =
    24 * base::Time::kSecondsPerHour;
const uint64_t kDebugFetchSubdivisionTargetingPing =
    5 * base::Time::kSecondsPerMinute;

}   // namespace

SubdivisionTargeting::SubdivisionTargeting(
    AdsImpl* ads)
    : ads_(ads) {
  DCHECK(ads_);
}

SubdivisionTargeting::~SubdivisionTargeting() = default;

bool SubdivisionTargeting::ShouldAllowForLocale(
    const std::string& locale) const {
  if (!IsSupportedLocale(locale)) {
    return false;
  }

  const std::string country_code = brave_l10n::GetCountryCode(locale);

  const std::string subdivision_targeting_code =
      GetAdsSubdivisionTargetingCode();

  const SupportedSubdivisionCodesSet subdivision_codes =
      kSupportedSubdivisionCodes.at(country_code);
  if (subdivision_codes.find(subdivision_targeting_code) ==
      subdivision_codes.end()) {
    return false;
  }

  return true;
}

bool SubdivisionTargeting::IsDisabled() const {
  const std::string subdivision_targeting_code =
      ads_->get_ads_client()->GetStringPref(
          prefs::kAdsSubdivisionTargetingCode);

  if (subdivision_targeting_code != "DISABLED") {
    return false;
  }

  return true;
}

void SubdivisionTargeting::MaybeFetchForLocale(
    const std::string& locale) {
  if (!IsSupportedLocale(locale)) {
    BLOG(1, "Ads subdivision targeting is not supported for " << locale
        << " locale");

    ads_->get_ads_client()->SetBooleanPref(
        prefs::kShouldAllowAdsSubdivisionTargeting, false);

    return;
  }

  if (IsDisabled()) {
    BLOG(1, "Ads subdivision targeting is disabled");
    return;
  }

  if (!ShouldAutoDetect()) {
    const std::string subdivision_targeting_code =
        ads_->get_ads_client()->GetStringPref(
            prefs::kAdsSubdivisionTargetingCode);

    BLOG(1, "Ads subdivision targeting is enabled for "
        << subdivision_targeting_code);

    return;
  }

  BLOG(1, "Automatically detecting ads subdivision");

  Fetch();
}

void SubdivisionTargeting::MaybeFetchForCurrentLocale() {
  const std::string locale =
      brave_l10n::LocaleHelper::GetInstance()->GetLocale();

  MaybeFetchForLocale(locale);
}

std::string SubdivisionTargeting::GetAdsSubdivisionTargetingCode() const {
  if (ShouldAutoDetect()) {
    return ads_->get_ads_client()->GetStringPref(
        prefs::kAutoDetectedAdsSubdivisionTargetingCode);
  }

  return ads_->get_ads_client()->GetStringPref(
      prefs::kAdsSubdivisionTargetingCode);
}

///////////////////////////////////////////////////////////////////////////////

bool SubdivisionTargeting::IsSupportedLocale(
    const std::string& locale) const {
  const std::string country_code = brave_l10n::GetCountryCode(locale);

  const auto iter = kSupportedSubdivisionCodes.find(country_code);
  if (iter == kSupportedSubdivisionCodes.end()) {
    return false;
  }

  return true;
}

void SubdivisionTargeting::MaybeAllowForLocale(
    const std::string& locale) {
  const bool should_allow = ShouldAllowForLocale(locale);

  ads_->get_ads_client()->SetBooleanPref(
      prefs::kShouldAllowAdsSubdivisionTargeting, should_allow);
}

bool SubdivisionTargeting::ShouldAutoDetect() const {
  const std::string subdivision_targeting_code =
      ads_->get_ads_client()->GetStringPref(
          prefs::kAdsSubdivisionTargetingCode);

  if (subdivision_targeting_code != "AUTO") {
    return false;
  }

  return true;
}

void SubdivisionTargeting::Fetch() {
  if (retry_timer_.IsRunning()) {
    return;
  }

  BLOG(1, "Fetch ads subdivision");
  BLOG(2, "GET /v5/getstate");

  GetSubdivisionUrlRequestBuilder url_request_builder;
  UrlRequestPtr url_request = url_request_builder.Build();
  BLOG(5, UrlRequestToString(url_request));
  BLOG(7, UrlRequestHeadersToString(url_request));

  const auto callback = std::bind(&SubdivisionTargeting::OnFetch, this, _1);
  ads_->get_ads_client()->UrlRequest(std::move(url_request), callback);
}

void SubdivisionTargeting::OnFetch(
    const UrlResponse& url_response) {
  BLOG(6, UrlResponseToString(url_response));
  BLOG(7, UrlResponseHeadersToString(url_response));

  bool should_retry = false;

  if (url_response.status_code / 100 == 2) {
    if (!url_response.body.empty()) {
      BLOG(1, "Successfully fetched ads subdivision");
    }

    if (!ParseJson(url_response.body)) {
      BLOG(1, "Failed to parse ads subdivision");
      should_retry = true;
    }
  } else if (url_response.status_code == 304) {
    BLOG(1, "Ads subdivision is up to date");
  } else {
    BLOG(1, "Failed to fetch ads subdivision");

    should_retry = true;
  }

  if (should_retry) {
    Retry();
    return;
  }

  retry_timer_.Stop();

  const std::string subdivision_targeting_code =
      GetAdsSubdivisionTargetingCode();
  BLOG(1, "Automatically detected ads subdivision targeting code as "
      << subdivision_targeting_code);

  const std::string locale =
      brave_l10n::LocaleHelper::GetInstance()->GetLocale();
  MaybeAllowForLocale(locale);

  FetchAfterDelay();
}

bool SubdivisionTargeting::ParseJson(
    const std::string& json) {
  base::Optional<base::Value> value = base::JSONReader::Read(json);
  if (!value || !value->is_dict()) {
    return false;
  }

  base::DictionaryValue* dictionary = nullptr;
  if (!value->GetAsDictionary(&dictionary)) {
    return false;
  }

  const std::string* country = dictionary->FindStringKey("country");
  if (!country || country->empty()) {
    return false;
  }

  const std::string* region = dictionary->FindStringKey("region");
  if (!region || region->empty()) {
    return false;
  }

  const std::string subdivision_code =
      base::StringPrintf("%s-%s", country->c_str(), region->c_str());

  ads_->get_ads_client()->SetStringPref(
      prefs::kAutoDetectedAdsSubdivisionTargetingCode,
          subdivision_code);

  return true;
}

void SubdivisionTargeting::Retry() {
  const base::Time time = retry_timer_.StartWithPrivacy(
      base::TimeDelta::FromSeconds(kRetryAfterSeconds),
          base::BindOnce(&SubdivisionTargeting::Fetch, base::Unretained(this)));

  BLOG(1, "Retry fetching ads subdivision " << FriendlyDateAndTime(time));
}

void SubdivisionTargeting::FetchAfterDelay() {
  const uint64_t ping = _is_debug ? kDebugFetchSubdivisionTargetingPing :
      kFetchSubdivisionTargetingPing;

  const base::TimeDelta delay = base::TimeDelta::FromSeconds(ping);

  const base::Time time = timer_.StartWithPrivacy(delay,
      base::BindOnce(&SubdivisionTargeting::Fetch, base::Unretained(this)));

  BLOG(1, "Fetch ads subdivision " << FriendlyDateAndTime(time));
}

}  // namespace ads