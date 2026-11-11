class BordersMissionControl < Formula
  env :std
  desc "JankyBorders with display-scoped Mission Control fading"
  homepage "https://github.com/thalesmello/JankyBorders"
  url "https://github.com/thalesmello/JankyBorders/archive/7606fd32b7e2dab3b2a45161dc663ddda3b0e6c4.tar.gz"
  version "1.9.0.1"
  sha256 "45a74ec83e570aae983ddf45c8b69d44d797767e6507f0543be922acc908c9bf"
  license "GPL-3.0-only"

  depends_on :macos

  conflicts_with "felixkratz/formulae/borders", because: "both install the borders executable and man page"

  def install
    system "make"
    system "/usr/bin/codesign", "--force", "--sign", "-",
           "--identifier", "local.jankyborders.mission-control", "bin/borders"
    bin.install "bin/borders"
    man1.install "docs/borders.1"
  end

  def caveats
    <<~EOS
      Enable Mission Control fading with:
        borders mission_control=on mission_control_fade=80

      The fade is time-based, not proportional to trackpad gesture progress.
      Accessibility permission enables early Mission Control notifications.
      If replacing upstream Borders, unlink it before installing this formula:
        brew unlink felixkratz/formulae/borders
    EOS
  end

  test do
    assert_match "borders-v1.9.0", shell_output("#{bin}/borders --version")
  end
end
