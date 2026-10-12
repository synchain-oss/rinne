# Contributing to Rinne

欢迎参与。Rinne（回环）是一对开源的 DAW 插件：Rinne Input 放插入链第一格，Rinne Output 放推子前区最后一格，两者之间的整段插件链就成了一个反馈环路。issue 与 PR 可以用中文或英文，维护者用你使用的语言回复。下面的规则以英文为准；开工之前请读完，尤其是「Changes that will be rejected」一节。

Rinne is a pair of open-source DAW plugins: **Rinne Input** goes in the first insert slot (the return end) and holds
all the controls; **Rinne Output** goes in the last pre-fader slot (the send end) and only handles grouping and
pairing. Everything between them on the insert chain becomes a feedback loop. Issues and pull requests in English or
Chinese are welcome.

## 0. Language and terms

| Term | Meaning |
| --- | --- |
| Rinne Input | return end, **first insert slot**; all controls |
| Rinne Output | send end, **last pre-fader slot**; group and pairing only |
| loop delay *D* | fixed delay of one trip around the loop |
| group A–H | which Input and Output form a pair |
| Panic | emergency cut of the loop; a UI action, not a parameter |

Do not use "Rinne Send", "Rinne Return" or "Loopback" as product names. If you know Synchain's SCVB: its Input and
Output point the other way.

## 1. Code of conduct

Rinne follows the code of conduct of the synchain-oss organisation
([synchain-oss/.github](https://github.com/synchain-oss/.github)). Report problems to **contact@synchain.ca**.

## 2. Developer Certificate of Origin

Every commit needs a `Signed-off-by:` line in **your own name and email**, which certifies the
[Developer Certificate of Origin](https://developercertificate.org/):

```sh
git commit -s
```

The last line of your pull request description is the same `Signed-off-by:` line. There is no CLA. Contributions
are accepted under the project licence, GPL-3.0-or-later: what comes in goes out under the same terms.

## 3. How to contribute

- Fork the repository, create a branch with any name except a long-lived one (`dev`, `prod`, `staging`, `main`,
  `master`, `feature/...`), and open the pull request against **`dev`**.
- While v1 is being built, issues and pull requests are limited to collaborators. The limit is lifted when v1 reaches
  `dev`.
- Workflows on fork pull requests run only after a maintainer approves them, and only the checks that need no
  secrets. The AI review runs when a maintainer comments `/review`.
- For anything bigger than a fix, open an issue first so the design can be agreed before you write the code.
- Commit subjects use `type(scope): description`, for example `fix(dsp): clamp the limiter ceiling`.
- Security problems: do not open a public issue; see [SECURITY.md](SECURITY.md).

## 4. Building and checking

- Build steps are in `docs/BUILDING.md` (added together with the build system). JUCE is pinned by `.juce-version` and
  `.juce-commit` and passed with `-DJUCE_PATH=<your JUCE checkout>`.
- Before opening a pull request, run the local gates (PowerShell 7) and the static checks (Node, version in
  `.node-version`):

  ```sh
  pwsh scripts/gates.ps1
  node scripts/run-checks.mjs
  ```

  Without a tier argument the gates script runs the lowest tier your diff needs: documentation and script changes
  run the quick tier, code changes also build and test.
- Every new source file carries an SPDX licence identifier line with the value `GPL-3.0-or-later` within its first
  five lines (checked by `scripts/check-spdx.mjs`).

## 5. Changes that will be rejected

These are not style preferences. Once a version is released they are stored in users' projects, or they protect
people's ears and speakers.

- **Changing a frozen contract without a contract change.** Plugin identity, parameter IDs, the shared-memory layout
  and the state format are frozen (`docs/contracts/`, `tests/golden/`). A pull request that touches them must add
  `docs/contract-changes/<YYYYMMDD>-<slug>.md` (see the template there) or state `contract-neutral: <reason>` in its
  description. The branch gate checks this.
- **Real-time violations.** No allocation, locks, I/O, logging, exceptions or blocking calls on the audio thread.
- **Weakening the safety defaults.** Feedback starts at 0, the limiter is on by default, every gain change is
  smoothed, and any failure silences the loop instead of making it louder.
- **Copying other people's code.** LGPL-3.0 code (e.g. LSP Plugins) is licence-compatible with GPL-3.0, but we do not
  copy it: it would add third-party copyright holders and licence notices to maintain. Study ideas only. Proprietary
  code and manuals (e.g. Blue Cat's) are copyrighted and must not be copied.
- **New dependencies or JUCE modules without the paperwork.** They need `THIRD-PARTY-NOTICES.md`, `LICENSES/`,
  `third_party/notices/`, `REUSE.toml` and `THIRD_PARTY_BY_MODULE` in `scripts/lib/rules.mjs` updated in the same
  pull request.
- **Network access.** The plugins work fully offline; the only allowed use of a URL is opening a link in the user's
  browser after a click.

## 6. Documentation

- Generated blocks in the documentation come from one source each. Change the source and re-run the generator in the
  same pull request; never edit a generated block by hand.
- Strong claims about behaviour ("does not change with...", "safe by default") need evidence registered in
  `docs/claims.json`.
- User-facing changes get a line in `CHANGELOG.md` once the project has its first public release.

## 7. Maintenance

Rinne has a single maintainer and is maintained on a best-effort basis. Expect a first response within about a week
(safety issues within 48 hours). Every comment on a pull request gets an answer, including those from review bots.
