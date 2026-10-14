# bottlesumo_ai verification report

Evidence source: active Fusion 360 document via local Fusion MCP. The active
saved document is `bottlesumo_ai v10`.

## Verified in Fusion

- One root chassis body with 92 chronological timeline features. The corrective
  pass adds three measured-envelope ToF backplates, six ToF M3 holes, 16 QRE
  capture rails, six ToF side-capture rails, and four symmetric motor cradle
  rails.
- 13 imported assembly occurrences: PCB, E326S battery, two motors, two
  wheels, three VL53L0X heads, and four QRE1113 heads.
- All 92 direct timeline features report healthy Fusion feature state
  (`health=0`).
- Chassis root envelope: 220 x 180 x 77.5 mm; imported wheel envelope reaches
  approximately x=±138.3 mm, so the complete assembled envelope is larger than
  the nominal floor plan and must be checked against the event's actual size
  rule before competition use.
- Wheel lowest point: approximately 0 mm at the chassis ground datum after
  axle correction.
- Four PCB pilot-hole sketch dimensions are present and each is driven by the
  `insert_pilot_diameter` user parameter. Change the parameter from 0.42 cm to
  0.50 cm for the alternate insert size.
- PCB is elevated above the E326S battery occurrence at z=35.5 mm; the four
  existing parametric boss pilots remain driven by `insert_pilot_diameter`.
- The E326S occurrence is rotated 90 deg about Z and centered at x=0, y=0 under
  the PCB. Its transformed envelope is approximately 61.6 x 89 x 24 mm.
- Final Fusion pair checks report zero volumetric interference for PCB↔chassis
  and E326S↔chassis. The motor↔wheel check reports non-zero intentional
  shaft/hub engagement (right: 0.278 cm³ aggregate, left: 0.311 cm³ aggregate),
  so this coupling still needs a physical free-rotation check after printing.
- All three VL53L0X occurrences have a -90 deg X rotation matrix, which maps the
  imported local optical +Z normal to world +Y (opponent-facing).
- All four QRE occurrences have a 180 deg X rotation matrix, mapping their
  optical face to world -Z (ground-facing); their boards sit over full-board
  floor windows and printed retention rails.
- The v10 QRE capture frames leave 0.9 mm per side around the measured 8 x 14
  mm board envelope. The v10 ToF cassettes leave 0.4 mm per side around the
  measured 25.22 x 10.92 mm board envelope and keep the two-hole row on the
  19.4 mm M3 pattern.
- Both motor occurrences now share the y=-22 mm center datum; the lower cradle
  rails rise to z=21 mm against the measured z=20.75 mm motor envelope.
- Current captures: `3D model/bottlesumo_ai/images/bottlesumo_ai-v10-top.png`,
  `bottlesumo_ai-v10-front.png`, `bottlesumo_ai-v10-back.png`,
  `bottlesumo_ai-v10-bottom.png`, and
  `bottlesumo_ai-v10-iso-top-right.png`.

## Not proven by Fusion geometry alone

- Exact motor shaft-to-wheel coupling and fastener fit.
- Exact motor-stand fastener-hole alignment; the supplied local stand archive
  could not be imported through the Fusion MCP endpoint, so the v10 cradle is
  an envelope-based printed interface rather than a proven stand bolt pattern.
- E326S physical slide insertion, connector bend radius, and retention force.
- Heat-set insert manufacturer's pilot recommendation and pull-out strength.
- PLA print success, layer strength, warping, and actual 2 mm clearance after
  printing.
- Finished robot mass, battery declaration voltage, and referee inspection.
- Fusion interference analysis was run pairwise. It reports substantial volumes
  for imported assemblies because nested component bodies include intentional
  shaft/hub and support contacts; the result is not a clean no-intersection
  proof. The motor/wheel coupling and ToF backplate contacts therefore remain a
  physical fit gate, not a completion claim.

## Save status

Fusion reports the active document saved as `bottlesumo_ai v10` in the `BottleSumo`
project. The saved document was re-opened/read successfully after save and is
not read-only. The latest saved captures are the `bottlesumo_ai-v10-*` images
listed above.
