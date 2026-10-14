# bottlesumo_ai assembly timeline

Status: Corrective Fusion timeline pass complete and saved as `bottlesumo_ai v10`;
imported-component placement, sensor orientation, and mounting-interface checks
are recorded below. Physical print and hardware fit checks remain required.

Latest Fusion MCP verification: the chassis root contains one solid body and 92
timeline features. The imported assembly is inside the 220 x 180 mm chassis
planform: wheels occupy approximately x=±108.5..±138.3 mm, with the axle at
36 mm above the ground datum. The three VL53L0X boards occupy x=-67.6..67.6 mm,
y=78.3..82.1 mm, z=64.5..75.5 mm. The four QRE1113 boards occupy the corner
windows at approximately x=±76..84 mm, y=±63..77 mm, z=2.6..5.9 mm.

## Design intent

Create one FDM-printed PLA chassis body for the BottleSumo robot, 220 x 180 mm
nominal footprint, with the E326S battery sliding into a front/rear-access slot
below a raised 100 x 90 mm PCB. The PCB is mounted on M3 brass hex standoffs.
Three VL53L0X heads form the front sensing arc. Four QRE1113 modules sit near
the four corners with 2-3 mm optical clearance.

## User parameters

| Parameter | Initial value | Purpose |
|---|---:|---|
| `chassis_length` | 220 mm | Front-to-rear footprint |
| `chassis_width` | 180 mm | Left-to-right footprint |
| `wall_thickness` | 3 mm | PLA nominal wall |
| `floor_thickness` | 3 mm | Structural chassis floor |
| `pcb_length` | 100 mm | PCB envelope |
| `pcb_width` | 90 mm | PCB envelope |
| `pcb_hole_edge_offset` | 5 mm | Hole-center offset on all four corners |
| `pcb_hole_diameter` | 3.4 mm | M3 PCB clearance starting value |
| `insert_pilot_diameter` | 4.2 mm | Default heat-set insert pilot; alternate 5.0 mm |
| `insert_boss_wall` | 3 mm | PLA around insert pilot |
| `pcb_clearance` | 5 mm | Battery/PCB and connector allowance |
| `qre_ground_gap` | 2.5 mm | Adjustable QRE optical gap |
| `tof_low_height` | 70 mm | Optical center target |
| `tof_yaw` | 25 deg | Reserved yaw parameter; current three-head baseline is normal to +Y |
| `battery_fit_clearance` | 1 mm | Per side of E326S slot envelope |
| `battery_retainer_clearance` | 5 mm | Cable/strap relief above pack |
| `qre_mount_clearance` | 0.9 mm | Per-side clearance for the imported QRE1113 envelope |
| `qre_rail_width` | 1.5 mm | Printed QRE capture rail width |
| `qre_rail_height` | 4.5 mm | Printed QRE capture rail height |
| `tof_mount_clearance` | 0.4 mm | Per-side clearance for the imported VL53L0X envelope |
| `tof_backplate_thickness` | 2.5 mm | Printed ToF backplate thickness |
| `tof_rail_width` | 2.0 mm | Printed ToF side-capture rail width |
| `motor_rail_height` | 18 mm | Printed motor cradle rise above the floor |

`pcb_standoff_height` is derived from the verified E326S height plus
`pcb_clearance`; it is not hard-coded until the battery envelope is checked.

## Ordered timeline features

1. **Parameters** — create the named user parameters above.
2. **Chassis master sketch** — constrain a centered 220 x 180 mm rectangle and
   mark front, rear, centerline, wheel contact line, PCB datum, and sensor datums.
3. **Base body extrusion** — extrude the PLA floor and perimeter walls as one
   chassis body; preserve 3 mm nominal walls.
4. **Motor mounting interfaces** — place the supplied 24GP motor stand and
   cut/bolt its mounting features; verify shaft and wheel alignment.
5. **Wheel reliefs** — cut wheel envelopes from the supplied High Grip Wheel
   reference while preserving the 2 mm competition clearance boundary.
6. **E326S battery slot** — create a captive slide-in pocket opening at the
   selected front/rear face; add a hard stop, cable channel, and removable
   strap/clamp interface.
7. **PCB datum and bosses** — locate four PCB hole centers at (5,5), (95,5),
   (5,85), and (95,85) mm; create PLA bosses with 3 mm minimum surrounding wall.
8. **Parametric insert holes** — cut boss pilots from `insert_pilot_diameter`,
   allowing 4.2 mm or 5.0 mm values; add M3 screw access and heat-set depth stops.
9. **Brass standoff stack** — place M3 hex standoffs above the bosses; derive
   their height from the battery envelope plus 5 mm clearance.
10. **Three-head ToF bracket** — add front-left, front-center, and front-right
    vertical backplates using the supplied VL53L0X model. The final occurrence
    transform is -90 deg about X (the requested 90-degree turn, with the sign
    chosen so the optical normal points +Y toward the opponent). The imported
    two-hole row is carried through as six M3 clearance holes at 19.4 mm total
    spacing per board; the optical center is 70 mm above the table.
11. **QRE corner mounts** — add four full-board floor windows and printed
    retention rails. Each imported QRE occurrence is rotated 180 deg about X so
    its optical face points -Z toward the table; the optical face is about
    2.6 mm above the table datum.
12. **Front contact wedge** — create a passive rounded front face that keeps
    continuous marked-front contact during a push without sharp edges.
13. **Cable routing and strain relief** — add protected channels from battery,
    motors, QRE modules, and ToF bracket to the PCB service area.
14. **Printability pass** — apply fillets/chamfers, check overhangs, orient
    bosses for vertical strength, and keep insert pilots free of elephant-foot
    interference.
15. **Inspection measurement** — verify static envelope, height, mass target,
   2 mm minimum clearance, battery voltage declaration, and no prohibited
   traction or sharp features.

16. **QRE hardware capture frames** — add four-sided printed capture frames
    around the measured 8 x 14 mm QRE envelopes. The existing full-board floor
    windows remain open below the optical face; the rails provide 0.9 mm per-side
    clearance and rise 4.5 mm above the floor datum.
17. **VL53L0X hardware cassettes** — add 2.5 mm rear backplates and 2.0 mm
    side-capture rails around each measured 25.22 x 10.92 mm board. The imported
    board remains at -90 degrees about X, with its optical normal toward +Y and
    its two-hole row aligned to the 19.4 mm M3 pattern.
18. **Motor cradle correction** — align both motor occurrences to the common
    y datum and add symmetric lower cradle rails sized to the supplied 30.5 mm
    motor envelope. Stand fastener-hole fit is still a physical verification
    gate because the local stand archive could not be imported through the MCP.

## Assembly sequence

1. Install motor stands and motors into the chassis.
2. Install wheels and verify free rotation without chassis rubbing.
3. Insert the E326S battery from the chosen access side and secure the strap.
4. Install M3 brass hex standoffs into the chassis bosses.
5. Mount the PCB, route the battery and motor wiring, and retain connectors.
6. Install the three ToF heads and four QRE modules.
7. Set sensor heights and yaw, then perform ground-gap and optical calibration.

## Verification gates

- Battery slot fit must be checked against the physical E326S pack.
- Motor stand and wheel references must be checked against the physical parts.
- Insert pilot diameter must follow the selected insert manufacturer's data.
- PCB must sit above the battery with connector and solder-joint clearance.
- No claim of competition readiness is made until the assembled robot is weighed
  and measured.

## Corrective placement record (v10)

- PCB: centered at z=35.5 mm; supplied 100 x 90 mm geometry is above the battery
  with zero volumetric PCB-to-chassis interference in the final Fusion check.
- E326S: rotated 90 deg about Z for a rear-access slide direction; the supplied
  61.6 x 89 x 24 mm transformed envelope starts at approximately z=4 mm and is
  centered under the PCB. The rear-open cavity leaves a 3 mm floor and gives
  zero volumetric battery-to-chassis interference in the final Fusion check.
- Motors: shaft axes are collinear with the wheel hubs at z=36 mm; right motor
  points +X and left motor is mirrored to point -X.
- Wheels: hubs are outside the chassis side walls and overlap the motor shafts;
  wheel lowest point is approximately z=0 mm.
- Motors: the final shaft/hub overlap is intentional mechanical engagement, not
  a no-contact clearance target; the interference analysis is recorded as an
  assembly-coupling check rather than a collision-free check.
- ToF: three vertical boards face +Y and are supported by positive-Z backplates,
  not the earlier negative-Z placeholder plates.
- QRE: four boards are upside-down over full-board openings and retained by
  printed side/rear rails.
