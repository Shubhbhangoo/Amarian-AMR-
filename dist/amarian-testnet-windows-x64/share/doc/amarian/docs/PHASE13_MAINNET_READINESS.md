# Phase 13 — Mainnet readiness

Phase 13 is a release gate, not another feature phase. The code being able to
build is necessary but is not evidence that a mainnet launch is safe. Every
gate below needs a dated artifact, a named owner, and a reproducible command or
review record.

The local audit is [`scripts/phase13_readiness.sh`](../scripts/phase13_readiness.sh).
It checks repository-controlled evidence only. A passing local audit does not
mean that Amarian is ready to launch.

## Gate status

| gate | requirement | evidence required | state |
|---|---|---|---|
| P13-01 | Consensus and protocol freeze candidate | versioned parameter record, genesis/network identifiers, final test vectors, and a written change-control rule | open |
| P13-02 | Independent security review | signed or attributable review covering consensus, networking, wallet, RPC, and cryptographic integration; findings and fixes tracked | open |
| P13-03 | Long-running testnet | independent nodes run continuously for weeks or months with mining, transactions, reorgs, upgrades, and incident records | open |
| P13-04 | Upgrade and migration procedure | documented activation rules, compatibility window, operator steps, rollback limits, and a rehearsal on testnet | open |
| P13-05 | Backup and recovery | tested wallet seed restore, wallet backup restore, node data backup restore, and recovery after simulated data loss | open |
| P13-06 | Release engineering | version policy, reproducible build record, signed checksums/artifacts, release notes, and distribution procedure | partial — local packaging exists; signing and distribution do not |
| P13-07 | Operational monitoring | documented metrics, log retention, disk/RSS/peer alerts, incident response, and an operator dashboard or equivalent | open |
| P13-08 | Mainnet launch decision | final parameters, launch height, compatibility matrix, support plan, and explicit go/no-go approval | blocked by P13-01 through P13-07 |

No gate is complete merely because its code path exists. In particular, local
tests cannot substitute for an external review or a long-running independent
testnet.

## Required evidence layout

When evidence is produced, keep it reviewable and immutable by release:

```text
docs/phase13/
  consensus-freeze.md
  security-review.md
  testnet-operation.md
  upgrade-migration.md
  recovery-drills.md
  release-record.md
  monitoring-runbook.md
  launch-decision.md
```

Each record should include the date, software commit, network parameters,
hardware or operators involved, exact commands, raw logs or artifact hashes,
observed failures, and the person responsible for the sign-off.

## First work package

The first Phase 13 work package is repository-controlled and can be completed
without claiming outside assurance:

1. Run the local readiness audit and preserve its output with the build record.
2. Turn the current consensus/network parameters into a freeze candidate.
3. Define the testnet topology, incident log, upgrade rehearsal, and recovery
   drill formats before the long-running testnet begins.
4. Prepare the external-review scope and threat-model handoff.
5. Add monitoring and release-signing procedures before any public launch claim.

Until the external and operational gates have evidence, the project remains a
development/testnet project and must not be presented as a launched monetary
network.
