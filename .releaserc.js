// The branches array is scoped to the branch actually being released rather
// than listing every line at once. Two hard constraints force this:
//
//   1. semantic-release caps `release`-type branches at 3 and globs branch
//      patterns against every branch on origin, so a `hotfix/*` pattern with
//      several live lines throws ERELEASEBRANCHES.
//   2. Giving the pattern a `range` (making it a maintenance branch) instead
//      forces every hotfix line to be strictly older than main's version, so a
//      current-line hotfix resolves to an empty range and dies with
//      EINVALIDNEXTVERSION.
//
// Scoping sidesteps both: one release branch per run, no ordering relative to
// sibling lines. GITHUB_REF_NAME is set for every GitHub Actions step; the
// fallback keeps local `semantic-release --dry-run` runs working.
const refName = process.env.GITHUB_REF_NAME ?? '';
const hotfixLine = /^hotfix\/(\d+\.(?:\d+|x)\.x)$/.exec(refName)?.[1];

// A scoped hotfix branch accepts patch/minor/major, so an unclamped `feat:`
// on hotfix/1.2.x would resolve to 1.3.0 and collide with a version already
// shipped from main. release-hotfix.yaml only cherry-picks fix:/perf:, but
// clamp here as well so a hand-pushed commit can't jump the line.
//
// Points at ./tools/release-first-parent-plugin.js, not
// @semantic-release/commit-analyzer directly: that wrapper recomputes the
// commit range with --first-parent before delegating to the real
// commit-analyzer, and also supplies generateNotes (see that file for why --
// a sync-merge PR can otherwise flood the changelog and bump calculation
// with thousands of already-shipped commits). A single array entry here
// registers both hook types, so no separate release-notes-generator entry
// is needed below.
const commitAnalyzer = hotfixLine
  ? [
      './tools/release-first-parent-plugin.js',
      {
        releaseRules: [
          { breaking: true, release: 'patch' },
          { type: 'feat', release: 'patch' },
        ],
      },
    ]
  : './tools/release-first-parent-plugin.js';

module.exports = {
  // 'main' is the stable release channel. It only ever advances by fast-
  // forwarding to dev's tip during a minor/major promotion (release-
  // semantic.yaml) — hotfixes never touch it directly, so it can't diverge
  // from dev between promotions.
  //
  // 'dev' is the integration channel and produces vX.Y.Z-rc.N prereleases.
  //
  // 'hotfix/X.Y.x' releases patches for ANY line, current or older, uniformly.
  branches: hotfixLine
    ? [{ name: refName, channel: hotfixLine }]
    : ['main', { name: 'dev', prerelease: 'rc' }],
  plugins: [
    commitAnalyzer,
    // Must follow commitAnalyzer's generateNotes hook: generateNotes outputs
    // concatenate in plugins-array order.
    './tools/release-notes-feature-audit-plugin.js',
    [
      '@google/semantic-release-replace-plugin',
      {
        replacements: [
          {
            files: ['CMakeLists.txt'],
            from: 'VERSION [0-9]+\\.[0-9]+\\.[0-9]+',
            // Strip prerelease suffix so CMake gets '1.5.0' not '1.5.0-rc.1'.
            // No results assertion: stable after RC is a no-op (version already set).
            to: "VERSION ${nextRelease.version.split('-')[0]}",
          },
        ],
      },
    ],
    [
      '@semantic-release/git',
      {
        assets: ['CMakeLists.txt', 'features/**/Shaders/Features/*.ini'],
        message: 'chore(release): ${nextRelease.version} [skip ci]',
      },
    ],
    [
      '@semantic-release/github',
      {
        draftRelease: true,
        assets: [],
        successComment: false,
        failComment: false,
        releasedLabels: false,
      },
    ],
  ],
};
