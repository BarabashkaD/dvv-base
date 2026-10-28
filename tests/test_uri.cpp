#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/uri.h"

namespace {

std::optional<std::string> paramOf(std::string_view text, std::string_view name) {
    const auto uri = dvv::parseUri(text).value();
    const auto value = uri.param(name);
    if (!value) {
        return std::nullopt;
    }
    return std::string(*value);
}

template <class T>
concept CanCallParam = requires(T&& uri) { std::forward<T>(uri).param("a"); };

}  // namespace

TEST_CASE("bare relative path means file:") {
    const auto uri = dvv::parseUri("clip_1920x1080_yuv420p10le.yuv").value();
    CHECK(uri.scheme == "file");
    CHECK(uri.path == "clip_1920x1080_yuv420p10le.yuv");
    CHECK(uri.params.empty());
}

TEST_CASE("Windows drive paths are paths, not schemes") {
    const auto uri = dvv::parseUri("X:\\media\\clip.yuv").value();
    CHECK(uri.scheme == "file");
    CHECK(uri.path == "X:\\media\\clip.yuv");
    CHECK(dvv::parseUri("X:/media/clip.yuv").value().path == "X:/media/clip.yuv");
}

TEST_CASE("bare paths keep percent signs and accept parameters") {
    const auto uri = dvv::parseUri("/data/100%.yuv?fmt=v210").value();
    CHECK(uri.path == "/data/100%.yuv");
    CHECK(uri.param("fmt") == "v210");
}

TEST_CASE("file: URIs with parameters") {
    const auto uri = dvv::parseUri("file:clip.yuv?fmt=v210&size=1920x1080&fps=50").value();
    CHECK(uri.scheme == "file");
    CHECK(uri.path == "clip.yuv");
    REQUIRE(uri.params.size() == 3);
    CHECK(uri.param("size") == "1920x1080");
    CHECK(uri.param("fps") == "50");
    CHECK_FALSE(uri.param("missing").has_value());
}

TEST_CASE("file: URIs are percent-decoded and accept an empty authority") {
    CHECK(dvv::parseUri("file:a%20b.yuv").value().path == "a b.yuv");
    CHECK(dvv::parseUri("file:///data/clip.yuv").value().path == "/data/clip.yuv");
    CHECK(dvv::parseUri("file:///X:/media/clip.yuv").value().path == "X:/media/clip.yuv");
    CHECK(dvv::parseUri("file://localhost/data/a.yuv").value().path == "/data/a.yuv");
    CHECK(dvv::parseUri("FILE:clip.yuv").value().scheme == "file");
}

TEST_CASE("remote authorities are unsupported") {
    const auto uri = dvv::parseUri("file://server/share/clip.yuv");
    REQUIRE_FALSE(uri.has_value());
    CHECK(uri.error().code == dvv::Errc::Unsupported);
}

TEST_CASE("synthetic: URIs") {
    const auto uri = dvv::parseUri("synthetic:gradient?w=33&h=17&fmt=v210").value();
    CHECK(uri.scheme == "synthetic");
    CHECK(uri.path == "gradient");
    CHECK(uri.param("w") == "33");
    CHECK(uri.param("impl") == std::nullopt);
}

TEST_CASE("lavfi: bodies are opaque") {
    const auto uri = dvv::parseUri("lavfi:testsrc2=s=3840x2160:r=60,format=yuv422p10le").value();
    CHECK(uri.scheme == "lavfi");
    CHECK(uri.body == "testsrc2=s=3840x2160:r=60,format=yuv422p10le");
    CHECK(uri.path.empty());
    CHECK(uri.params.empty());
}

TEST_CASE("synthetic: impl parameter") {
    const auto uri = dvv::parseUri("synthetic:gradient?impl=cpu").value();
    CHECK(uri.param("impl") == "cpu");
}

TEST_CASE("opaque schemes keep the body, hierarchical schemes leave it empty") {
    CHECK(dvv::parseUri("lavfi:a?b=c&d").value().body == "a?b=c&d");
    CHECK(dvv::parseUri("lavfi:a?b=c&d").value().params.empty());
    CHECK(dvv::parseUri("file:a.yuv").value().body.empty());
    CHECK(dvv::parseUri("synthetic:gradient").value().body.empty());
    CHECK(dvv::parseUri("a.yuv").value().body.empty());
    // path and params are the whole meaning, so equal meaning compares equal
    CHECK(dvv::parseUri("a b.yuv").value() == dvv::parseUri("file:a%20b.yuv").value());
    CHECK(dvv::parseUri("file:a.yuv").value() == dvv::parseUri("FILE:a.yuv").value());
}

TEST_CASE("param needs an lvalue") {
    static_assert(!CanCallParam<dvv::Uri>);
    static_assert(!CanCallParam<const dvv::Uri>);
    static_assert(CanCallParam<dvv::Uri&>);
    static_assert(CanCallParam<const dvv::Uri&>);
}

TEST_CASE("invalid URIs") {
    CHECK(dvv::parseUri("").error().code == dvv::Errc::Format);
    CHECK(dvv::parseUri("file:a%2").error().code == dvv::Errc::Format);
    CHECK(dvv::parseUri("file:a%zz.yuv").error().code == dvv::Errc::Format);
    CHECK(dvv::parseUri("file:?fmt=v210").error().code == dvv::Errc::Format);
}

TEST_CASE("malformed escapes and NUL are format errors") {
    for (const char* text :
         {"file:a?k=%", "file:a?k=%%", "file:a?k=a%2", "file:a?k=%zz", "file:a?k=%00",
          "file:a?%00=v", "file:a?%=v", "synthetic:g?k=%0", "a.yuv?k=%", "a.yuv?k=%00",
          "file:a%00b", "file:%00", "file:a%", "file:a%%", "file:a%0"}) {
        INFO(text);
        const auto uri = dvv::parseUri(text);
        REQUIRE_FALSE(uri.has_value());
        CHECK(uri.error().code == dvv::Errc::Format);
    }
    CHECK(dvv::percentDecode("%00").error().code == dvv::Errc::Format);
    CHECK(dvv::percentDecode("a%00").error().code == dvv::Errc::Format);
}

TEST_CASE("raw NUL bytes are format errors") {
    const char bare[] = "a\0b";
    const char file[] = "file:a\0b";
    const char query[] = "synthetic:x?k=a\0b";
    const char opaque[] = "lavfi:a\0b";
    for (const std::string_view text :
         {std::string_view{bare, sizeof bare - 1}, std::string_view{file, sizeof file - 1},
          std::string_view{query, sizeof query - 1}, std::string_view{opaque, sizeof opaque - 1}}) {
        INFO(text.size());
        const auto uri = dvv::parseUri(text);
        REQUIRE_FALSE(uri.has_value());
        CHECK(uri.error().code == dvv::Errc::Format);
    }
}

TEST_CASE("empty names and empty bodies are format errors") {
    for (const char* text : {"file:a?=v", "file:a?a=1&=v", "a.yuv?=v", "lavfi:", "LAVFI:",
                             "file://", "file://localhost", "synthetic://", "file:"}) {
        INFO(text);
        const auto uri = dvv::parseUri(text);
        REQUIRE_FALSE(uri.has_value());
        CHECK(uri.error().code == dvv::Errc::Format);
    }
}

TEST_CASE("remote authorities stay unsupported") {
    const auto uri = dvv::parseUri("file://server/x");
    REQUIRE_FALSE(uri.has_value());
    CHECK(uri.error().code == dvv::Errc::Unsupported);
}

TEST_CASE("query rules") {
    const auto empty_segments = dvv::parseUri("file:a?&&x=1&&y=2&").value();
    REQUIRE(empty_segments.params.size() == 2);
    CHECK(empty_segments.param("x") == "1");
    CHECK(empty_segments.param("y") == "2");

    const auto no_equals = dvv::parseUri("file:a?a").value();
    REQUIRE(no_equals.params.size() == 1);
    CHECK(no_equals.param("a") == "");
    CHECK(no_equals.param("a").has_value());

    const auto empty_value = dvv::parseUri("file:a?a=").value();
    REQUIRE(empty_value.params.size() == 1);
    CHECK(empty_value.param("a") == "");
    CHECK(empty_value == dvv::parseUri("file:a?a").value());

    CHECK(paramOf("file:a?a=b=c", "a") == "b=c");
    CHECK(dvv::parseUri("file:a?").value().params.empty());

    const auto duplicates = dvv::parseUri("file:a?k=1&j=2&k=3").value();
    REQUIRE(duplicates.params.size() == 3);
    CHECK(duplicates.params[0].second == "1");
    CHECK(duplicates.params[2].second == "3");
    CHECK(duplicates.param("k") == "1");

    CHECK(paramOf("file:a?k=a+b", "k") == "a+b");
    CHECK(paramOf("a.yuv?k=1&k=2", "k") == "1");
}

TEST_CASE("hex digits decode the same in either case") {
    CHECK(dvv::percentDecode("%2f%2F%aB%Ab").value() == dvv::percentDecode("%2F%2f%Ab%aB").value());
    CHECK(dvv::percentDecode("%2f").value() == "/");
    CHECK(dvv::percentDecode("%2F").value() == "/");
    CHECK(paramOf("file:a?k=%c3%A9", "k") == "\xC3\xA9");
}

TEST_CASE("Windows drive letters in file: URIs") {
    for (const char* text : {"file:/X:/dir/a.yuv", "file:///X:/dir/a.yuv", "file:///X%3A/dir/a.yuv",
                             "file:/X%3a/dir/a.yuv", "file://localhost/X:/dir/a.yuv",
                             "file:X:/dir/a.yuv", "synthetic:/X:/dir/a.yuv"}) {
        INFO(text);
        CHECK(dvv::parseUri(text).value().path == "X:/dir/a.yuv");
    }
    CHECK(dvv::parseUri("file:/x:\\dir").value().path == "x:\\dir");
    CHECK(dvv::parseUri("file:/X:").value().path == "X:");
    CHECK(dvv::parseUri("file:///X%3A").value().path == "X:");
    // not a drive: unchanged
    CHECK(dvv::parseUri("file:/X:a").value().path == "/X:a");
    CHECK(dvv::parseUri("file:/XY:/a").value().path == "/XY:/a");
    CHECK(dvv::parseUri("file:/1:/a").value().path == "/1:/a");
    CHECK(dvv::parseUri("file:////X:/a").value().path == "//X:/a");
    // bare paths are never changed
    CHECK(dvv::parseUri("/X:/dir/a.yuv").value().path == "/X:/dir/a.yuv");
}

TEST_CASE("formatUri round-trips") {
    for (const char* text : {"file:clip.yuv?fmt=v210&fps=50",
                             "file:a%20b.yuv",
                             "synthetic:gradient?w=33&h=17",
                             "synthetic:gradient?w=33&fmt=v210",
                             "lavfi:testsrc2=s=64x64",
                             "file:X:/media/clip.yuv",
                             "file:///data/clip.yuv",
                             "a b.yuv",
                             "//srv/share/a.yuv",
                             "file:%2F%2Fsrv%2Fshare",
                             "file:/X:/dir/a.yuv",
                             "file:///X%3A/dir/a.yuv",
                             "file://LOCALHOST/data/a.yuv",
                             "file:a?k=a%26b%3Dc",
                             "synthetic:gradient?k=a%26b%3Dc&e&k=2&x=",
                             "/X:/dir/a.yuv",
                             "/X:",
                             "/X:/a?k=v",
                             "/data/100%.yuv?fmt=v210",
                             "file:a%3Fb%23c%25d",
                             "file:%C3%A9.yuv?n=%C3%A9",
                             "FILE:Clip.yuv",
                             "X:\\media\\clip.yuv",
                             "file:///",
                             "file:////",
                             "file:a?k=@x/y:z\\w",
                             "//"}) {
        INFO(text);
        const auto uri = dvv::parseUri(text).value();
        const auto again = dvv::parseUri(dvv::formatUri(uri));
        REQUIRE(again.has_value());
        CHECK(*again == uri);
    }
    CHECK(dvv::formatUri(dvv::parseUri("file:a%20b.yuv").value()) == "file:a%20b.yuv");
    CHECK(dvv::formatUri(dvv::parseUri("//srv/share/a.yuv").value()) == "file:////srv/share/a.yuv");
    CHECK(paramOf("synthetic:gradient?k=a%26b%3Dc", "k") == "a&b=c");
}

TEST_CASE("formatUri keeps '/', ':', '@' and '\\' in query values") {
    const auto uri =
        dvv::parseUri("file:a.yuv?fmt=%40X%3A%5Cf%2Fmy.json&fps=30000%2F1001&k=a%26b%3Dc%23%20+")
            .value();
    CHECK(dvv::formatUri(uri) ==
          "file:a.yuv?fmt=@X:\\f/my.json&fps=30000/1001&k=a%26b%3Dc%23%20%2B");
    CHECK(dvv::parseUri(dvv::formatUri(uri)).value() == uri);
}

TEST_CASE("formatUri query values round-trip with kept and encoded characters") {
    const std::string value = "@X:\\d/f:1@2 &=%#+?";
    for (const char* text : {"file:a.yuv", "synthetic:gradient", "/X:/a.yuv", "X:\\a.yuv"}) {
        INFO(text);
        auto uri = dvv::parseUri(text).value();
        uri.params = {{"k", value}, {"fps", "30000/1001"}};
        const auto formatted = dvv::formatUri(uri);
        CHECK(formatted.ends_with("?k=@X:\\d/f:1@2%20%26%3D%25%23%2B%3F&fps=30000/1001"));
        CHECK(dvv::parseUri(formatted).value() == uri);
    }
}

TEST_CASE("percent encoding") {
    CHECK(dvv::percentEncode("a b&c") == "a%20b%26c");
    CHECK(dvv::percentDecode("a%20b%26c").value() == "a b&c");
}

TEST_CASE("percentEncode always encodes the percent sign") {
    CHECK(dvv::percentEncode("100%", "%") == "100%25");
    CHECK(dvv::percentDecode(dvv::percentEncode("a%20b", "%/")).value() == "a%20b");
}

TEST_CASE("every byte 1..255 survives an encode and decode round trip") {
    for (int byte = 1; byte <= 255; ++byte) {
        const std::string text(1, static_cast<char>(byte));
        INFO(byte);
        CHECK(dvv::percentDecode(dvv::percentEncode(text)).value() == text);
        CHECK(dvv::percentDecode(dvv::percentEncode(text, "")).value() == text);
    }
}
