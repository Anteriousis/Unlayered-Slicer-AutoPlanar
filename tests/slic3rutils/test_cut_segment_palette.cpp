#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/3DScene.hpp"

#include <array>
#include <cmath>

// Declarative cut planes: the section palette. The Prepare tab's chunk preview and the G-code
// preview both colour a section by calling this one function with the segment index, so its output
// is a cross-tab contract: section N must be the same colour in both, on every platform, forever.

TEST_CASE("Section palette keeps its first sixteen colours", "[cutplanes]") {
    // These are the ORCA filament palette entries the feature shipped with. Changing one would
    // recolour existing projects' previews, so they are pinned.
    const std::array<std::array<int, 3>, 16> expected = { {
        { 0x00, 0xC1, 0xAE }, { 0xF4, 0xE2, 0xC1 }, { 0xED, 0x1C, 0x24 }, { 0x00, 0xFF, 0x7F },
        { 0xF2, 0x67, 0x22 }, { 0xFF, 0xEB, 0x31 }, { 0x78, 0x41, 0xCE }, { 0x11, 0x58, 0x77 },
        { 0xED, 0x1E, 0x79 }, { 0x2E, 0xBD, 0xEF }, { 0x34, 0x5B, 0x2F }, { 0x80, 0x00, 0x80 },
        { 0xFA, 0x81, 0x73 }, { 0x80, 0x00, 0x00 }, { 0xF7, 0xB7, 0x63 }, { 0xA4, 0xC4, 0x1E },
    } };
    for (size_t i = 0; i < expected.size(); ++ i) {
        const Slic3r::ColorRGBA c = cut_segment_color(i);
        CHECK(int(std::lround(c.r() * 255.f)) == expected[i][0]);
        CHECK(int(std::lround(c.g() * 255.f)) == expected[i][1]);
        CHECK(int(std::lround(c.b() * 255.f)) == expected[i][2]);
        CHECK(c.a() == 1.f);
    }
}

TEST_CASE("Section palette does not repeat past sixteen", "[cutplanes]") {
    // It used to wrap (idx % 16), so section 17 was drawn in section 1's colour -- and two sections
    // meeting at a cut plane could come out identical.
    constexpr size_t N = 96;
    std::array<Slic3r::ColorRGBA, N> colors;
    for (size_t i = 0; i < N; ++ i)
        colors[i] = cut_segment_color(i);

    auto channel_distance = [](const Slic3r::ColorRGBA &a, const Slic3r::ColorRGBA &b) {
        return std::max({ std::abs(a.r() - b.r()), std::abs(a.g() - b.g()), std::abs(a.b() - b.b()) });
    };

    for (size_t i = 0; i < N; ++ i)
        for (size_t j = i + 1; j < N; ++ j)
            CHECK(channel_distance(colors[i], colors[j]) > 0.f);

    // Sections that meet are consecutively numbered far more often than not (the partition is
    // indexed by a BFS from the root), so neighbours in index must be clearly, not marginally,
    // distinguishable.
    for (size_t i = 0; i + 1 < N; ++ i)
        CHECK(channel_distance(colors[i], colors[i + 1]) > 0.15f);
}

TEST_CASE("Section palette is stateless", "[cutplanes]") {
    // Unlike Plater::get_next_color_for_filament(), asking twice must give the same answer --
    // otherwise a section would change colour on every preview rebuild.
    for (size_t i = 0; i < 40; ++ i)
        CHECK(cut_segment_color(i) == cut_segment_color(i));
}
