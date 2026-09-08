# Amarian Website Design Specification

## Product

Amarian is an independent proof-of-work blockchain built from scratch in C++23. The site is a technical project homepage, not a token-sale or investment page.

## Design direction

**Visual language:** engineered, mineral, precise, dark. The visual metaphor is a cut diamond under laboratory light, expressed through facets, grids, sharp geometry and restrained glow rather than generic crypto neon.

**Tone:** serious engineering with confidence. Avoid hype, price language, moon imagery, trading UI and fake decentralisation claims.

## Color tokens

- Background: near-black graphite
- Surface: charcoal panels
- Primary text: warm white
- Secondary text: cool gray
- Accent: diamond white / icy blue
- Positive: muted green
- Warning: amber

Accent is used sparingly for active states, links, data highlights and the chain visualization.

## Typography

Use a modern system sans stack for headings and body. Use a monospace stack for hashes, commands, code, metrics and architecture labels. Large display typography should be compact and technical rather than editorial.

## Layout

- Full-width dark canvas
- 1200px maximum content width
- Generous vertical spacing
- Thin 1px borders
- Dense technical cards alternating with large open sections
- Responsive single-column collapse below tablet widths

## Hero

Headline: **Digital diamond.**

Supporting line: **An independent proof-of-work blockchain built from scratch in C++23.**

Primary CTA: GitHub
Secondary CTA: Read the protocol

Hero visual: animated abstract chain of blocks/facets. No fake live network statistics.

## Sections

1. Hero and project status
2. Core properties: fixed supply, SHA-256d PoW, UTXO, integer money, PQ readiness
3. Architecture with dependency flow
4. Node capabilities and current status
5. Post-quantum design explanation
6. Supply/economics with exact parameters
7. Verification and engineering discipline
8. Developer quick start
9. Documentation index
10. Footer with explicit engineering-project disclaimer

## Interaction level

L2/L3. Motion should explain structure, not decorate it. Use scroll reveal, hover elevation, animated architecture lines and subtle block/facet movement. Respect `prefers-reduced-motion`.

## Content rules

- Never imply monetary value or investment potential.
- Clearly distinguish completed functionality from future work.
- State that phases 0-12 are complete and Phase 13 mainnet readiness has not started.
- Do not call the project production-secure.
- Keep AMR described as a working/provisional ticker because the repository documents an existing NYSE ticker conflict.

## Accessibility

- Semantic headings and landmarks
- Keyboard-visible focus states
- Sufficient contrast
- Buttons and links have descriptive labels
- Reduced-motion mode disables decorative animation

## Quality bar

The final page should feel like a serious open-source protocol project, not a generic SaaS landing page or memecoin website. Every visual should communicate either architecture, verification, scarcity, or engineering discipline.
