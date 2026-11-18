# Source provenance

| Component | Revision | Terms |
| --- | --- | --- |
| [ModernGekko](https://github.com/ExpansionPak/ModernGekko) | `a2e4e2b026a68d4848f20ebf16f0e0ddeb573957` | GPL-3.0-or-later as documented by upstream |
| [RecompCore](https://github.com/ExpansionPak/RecompCore), ModernGekko’s `vendor/dolphin` | `cf8ceb81b47d6172164cd0572f57aa7624719f33` | Per-file SPDX terms, predominantly GPL-2.0-or-later, with an aggregate compatible with GPLv3 |
| [DolRecomp](https://github.com/ExpansionPak/DolRecomp) used to generate the Windows preview module | `02e27aca1c408cfcb2a4079a3a5b5ab3ae39d340` | GPL-3.0 |
| [decomp-toolkit](https://github.com/encounter/decomp-toolkit) used for disc extraction | `v1.8.4` | MIT OR Apache-2.0 |

`patches/ModernGekko.patch` applies at the ModernGekko root. `patches/RecompCore.patch` applies against the pinned RecompCore checkout. The patched source retains upstream copyright and license notices. The repository includes a [GPLv3 license text](../COPYING). See the pinned upstream `LICENSE`, `COPYING`, and `LICENSES` for the components and per-file notices. [ModernGekko provenance](https://github.com/ExpansionPak/ModernGekko/blob/a2e4e2b026a68d4848f20ebf16f0e0ddeb573957/PROVENANCE.md) · [RecompCore license notes](https://github.com/ExpansionPak/RecompCore/blob/cf8ceb81b47d6172164cd0572f57aa7624719f33/COPYING)

The build derives the AOT module locally from the user’s `sys/main.dol`. The verified PAL executable SHA-256 is `2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe`.
