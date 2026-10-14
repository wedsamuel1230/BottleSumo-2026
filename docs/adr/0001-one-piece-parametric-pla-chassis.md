# ADR 0001: One-piece parametric PLA chassis with raised PCB

## Status

Accepted baseline

## Decision

Use one FDM-printed PLA chassis body with 3 mm nominal walls. Mount the 100 x
90 mm PCB above the E326S battery using M3 brass hex standoffs. The battery
slides into a front- or rear-access slot. Chassis insert pilots are controlled
by a parameter that supports either a 4.2 mm or 5.0 mm heat-set insert.

## Rationale

This preserves a single printable structural body while keeping the PCB and
battery serviceable. A parametric pilot avoids committing the chassis to one
insert supplier before its fit recommendation is known. The raised PCB also
provides room for the battery and cable routing without adding a printed upper
deck.

## Trade-offs

The one-piece body is less convenient to replace than a split tub/deck design,
and PLA is less impact-tolerant than tougher FDM materials. These risks are
accepted for the current prototype and must be checked during print and fit
validation.
