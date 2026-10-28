# Privacy architecture (developers)

How Unlayered Slicer decides what optional data may leave the computer, since 0.1.6. What is sent is
listed in [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md). The policy text and the legal review list are
in [docs/privacy/](docs/privacy/), and every outbound request is listed in
[docs/privacy/NETWORK_TRAFFIC.md](docs/privacy/NETWORK_TRAFFIC.md).

## The rules the code keeps

1. **Fail closed.** Optional diagnostics run only on a saved, valid, explicit "share" (or an approved
   preselection, see 5). No record, an unreadable record, a record for an older notice, or anything
   unexpected means off. Before the choice nothing is collected, queued or kept, not just "not sent".
2. **Saved before applied.** The privacy page writes the record and saves the config first, then calls
   `UsageTelemetry::refresh()`.
3. **Asked at every send.** `UsageTelemetry::send()` checks permission for each request, not only at
   start-up. A withdrawal clears the queue and `pending.json`, and failed in-flight bodies are not kept.
4. **No always wins.** A decline survives updates, new notice versions and country changes. Only a
   "share" can need asking again.
5. **No silent preselection.** A preselected "share" exists only where `policy_for()` allows it
   (United States, with `LegalApprovals::us_preselect_share`, ON since 2026-10-14). Only there is it
   marked Recommended. Keeping it is recorded as `preselected_confirmed`, which stops counting as
   soon as the approval is withdrawn or the country changes. It is never reported as an explicit
   choice.
5a. **The Slicing improvement program is its own, explicit permission.** A separate box under
   "Share", never ticked in advance anywhere and never implied by a preselected "share".
   `program_permitted()` requires diagnostics granted, the box ticked, and the current notice.
   Every batch carries `program`, and the site keeps development and training records only from
   `program: true` batches. Joining or leaving drops whatever was queued or pending under the
   other answer.
6. **The organisation wins.** `UNLAYERED_NO_TELEMETRY` (any non-empty value) turns diagnostics off
   whatever the record says, and the privacy question is not asked.
7. **Allowlist, not blocklist.** Every event goes through `DiagnosticsAllowlist::filter()`. Unknown
   events and properties are dropped, and every string must pass `safe_string()` (no paths, model,
   project or G-code file names, e-mail addresses or line breaks).
8. **Models are separate.** Diagnostics consent never authorises a model upload. Any upload goes
   through `ModelUploadConsentDialog::ask()`. The authorisation it returns is single-use, and valid
   only for the file it names.

## Pieces

| File | What it does |
|---|---|
| `src/slic3r/Utils/PrivacyConsent.{hpp,cpp}` | The record (`privacy_consent` in the app config, strict JSON), the regional table `places()` (the region page's list), `policy_for()`, `LegalApprovals`, `status()` / `diagnostics_permitted()`, legacy migration, and the model-upload request and authorisation types. Pure and unit-tested. |
| `src/slic3r/Utils/DiagnosticsAllowlist.{hpp,cpp}` | Events, their properties, the tally prefixes, and the value check. |
| `src/slic3r/Utils/PrivacyChoiceStats.{hpp,cpp}` | Privacy-choice statistics: strict schema, per-flow de-duplication (`FlowCounter`), and a sender that is **disabled** (no legal approval, and `ENDPOINT` is empty). |
| `src/slic3r/Utils/UsageTelemetry.{hpp,cpp}` | Diagnostics. `start()` migrates and applies the record, `refresh()` re-applies it after any change, and `record()` / `count()` filter through the allowlist. Crash markers carry `consented`, and only a consented session's crash is reported. |
| `src/slic3r/GUI/PrivacySetupDialog.{hpp,cpp}` | `PrivacyFlow`, the message handler for the region and privacy pages, shared by the setup wizard and `PrivacySetupDialog` (after an update, and from Preferences). It saves the record and counts the page and the decision. |
| `src/slic3r/GUI/ModelUploadConsentDialog.{hpp,cpp}` | The per-model upload question. Used by the Universe tab's `slicer.exportProject` (`WebAppBridge.cpp`). |
| `resources/web/guide/11/` | "Where do you live?": one click among the ten places in `PrivacyConsent::places()` (radio group, two columns, nothing preselected). Each place is a record's region + country and decides the rules. |
| `resources/web/guide/13/` | "Help us build a better slicer": "Keep my data private" / "Share usage diagnostics", the Slicing improvement program box under Share, and the policy link. |
| `resources/web/privacy/slicer-privacy-policy.html` | The versioned policy. A reviewed translation goes next to it as `slicer-privacy-policy.<lang>.html` (`fr-CA`, `es-MX`) and is picked by `PrivacyFlow::policy_file()`. |
| `src/slic3r/GUI/Preferences.cpp` | Online ▸ Privacy: the switch, current choice and where you live, Change, policy link, model note. |
| `src/slic3r/GUI/GUI_App.cpp` | `ask_privacy_choice_once()`: opens the privacy pages after start-up when there is no valid choice (at most 3 times). |

Setup flow: welcome (1) → where you live (11) → privacy (13; saved on
Continue) → printers (21) → filaments → last page (4unlayered: stealth mode, Bambu plug-in). Nothing
optional is sent while the wizard is open (`GUI_App` timer gate).

## Changing things

- **New diagnostic:** add it to `DiagnosticsAllowlist.cpp`, to [USAGE_DIAGNOSTICS.md](USAGE_DIAGNOSTICS.md)
  and to the policy. A property that is not listed is silently dropped, and a warning is logged.
- **New wording on the privacy page that needs a fresh answer:** bump `NOTICE_VERSION`. Existing
  "share" records then count as `NeedsNewChoice` and are asked again; declines stay.
- **New place on the region page or new rule:** change `places()` / `policy_for()` only, and add a test case.
- **Turning on a legal approval:** only with counsel's written sign-off (see
  [docs/privacy/LEGAL_REVIEW_CHECKLIST.md](docs/privacy/LEGAL_REVIEW_CHECKLIST.md)), in
  `legal_approvals()`. These are not settings.
- **Privacy-choice statistics:** they need the approval plus a deployed endpoint that adds into
  aggregate counters, rejects unknown fields, and stores no events, IP addresses or identifiers. Then
  set `ENDPOINT`.
- **A feature that uploads a model:** build a complete `ModelUploadRequest` (`request_complete()`
  must hold), call `ModelUploadConsentDialog::ask()`, and `consume()` the result for the exact file
  before sending. Keep to `ai_training_allowed()`.

## Tests

`tests/slic3rutils/test_privacy_consent.cpp` (`[Privacy]`) and
`tests/slic3rutils/test_usage_telemetry.cpp` (`[UsageTelemetry]`):

```
build\tests\slic3rutils\Release\slic3rutils_tests.exe "[Privacy],[UsageTelemetry]"
```

They cover:
- regional rules for every place on the region page;
- record parsing and fail-closed;
- preselection that never passes for explicit consent;
- country change;
- legacy migration (an old explicit off is kept; the old default on is not consent, and fresh installs are asked);
- nothing collected before a choice;
- withdrawal discarding queued and pending data;
- restart;
- `UNLAYERED_NO_TELEMETRY`;
- the allowlist and value checks;
- redaction;
- crash markers from sessions without consent;
- statistics schema, de-duplication and disabled transmission;
- single-use model-upload authorisation.
