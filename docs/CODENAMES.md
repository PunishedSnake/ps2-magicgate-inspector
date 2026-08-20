# Release codenames

PS2 Memory Card Inspector releases use codenames based on detectives, police officers and investigators — famous, obscure, serious or slightly ridiculous. The name should fit the character of the milestone rather than being assigned randomly.

## Current plan

| Version | Codename | Why it fits |
| --- | --- | --- |
| v0.1.0 | **Columbo** | The first standalone build keeps asking the card one more question and cares more about inconsistencies than appearances. |
| v0.2.0 | **Briscoe** | Lennie Briscoe: practical, procedural, and a good fit for better error evidence and report handling. |
| v0.3.0 | **Poirot** | Capability fingerprinting and MagicGate investigation are about identifying the tiny details that do not quite match. |
| v0.4.0 | **Kojak** | Recovery work should be direct and unsentimental, but still know exactly what it is doing. |
| v0.5.0 | **Dale Cooper** | Reporting/UI work can afford to become a little friendlier and stranger without compromising the evidence. |
| v1.0.0 | **Inspector Gadget** | Reserved for the stable release for reasons requiring no further explanation. |

These names are proposed as the initial canon. Intermediate versions can be inserted without forcing every candidate below into a numbered slot.

## Candidate pool

Useful future names:

- **Sherlock** / **Holmes** — deep protocol or forensic analysis;
- **Marple** — quiet consistency checks and anomaly detection;
- **Somerset** — mature forensic/reporting milestone;
- **Marge Gunderson** — reliability and careful evidence gathering;
- **Harry Bosch** — low-level investigative work;
- **John Munch** — oddball compatibility cases and unexplained card behavior;
- **Frank Drebin** — an intentionally chaotic experimental branch, if we ever deserve one;
- **Clarice Starling** — deeper forensic inspection;
- **Scully** — skeptical verification of claims made by weird hardware;
- **L** — narrow, obsessive protocol investigation;
- **Benoit Blanc** — polished diagnostics and explanatory reports;
- **Zenigata** — persistent pursuit of cards that keep escaping normal classification;
- **Dick Gumshoe** — perfect for a scrappy developer/debug build;
- **Adachi** — probably best avoided for a release involving trusted conclusions.

## Naming rules

1. Stable numbered releases get one codename each.
2. Patch releases inherit the parent codename: `0.2.1 Briscoe` remains Briscoe.
3. Nightly/CI artifacts do not receive new codenames.
4. A codename should not imply a feature the release does not contain.
5. Names are project flavor, not an excuse to hide semantic versioning; the numeric version remains authoritative.
6. **Inspector Gadget stays reserved for 1.0.0.**
