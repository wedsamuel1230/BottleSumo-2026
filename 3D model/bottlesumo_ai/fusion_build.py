"""Bootstrap scaffold for bottlesumo_ai.

The authoritative assembly is the saved Fusion document ``bottlesumo_ai v10``.
This file preserves the early parametric scaffold used to start the timeline;
it does not import the hardware occurrences or reproduce the corrective v10
placement pass. Do not rerun it over the saved v10 document unless intentionally
starting a new candidate.
"""

import adsk.core, adsk.fusion

def run(_context: str):
    app = adsk.core.Application.get()
    ui = app.userInterface
    docs = app.documents
    doc = docs.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    design = adsk.fusion.Design.cast(app.activeProduct)
    # Fusion keeps the root component name tied to the document; the design
    # identity is carried by the named features and the saved document title.

    # All dimensions are expressed in centimeters because Fusion's internal unit is cm.
    params = design.userParameters
    def p(name, value, units, comment):
        params.add(name, adsk.core.ValueInput.createByString(value), units, comment)
    p('chassis_length', '22 cm', 'cm', '220 mm front-to-rear footprint')
    p('chassis_width', '18 cm', 'cm', '180 mm left-to-right footprint')
    p('wall_thickness', '0.3 cm', 'cm', '3 mm PLA nominal wall')
    p('floor_thickness', '0.3 cm', 'cm', '3 mm structural floor')
    p('pcb_hole_edge_offset', '0.5 cm', 'cm', '5 mm PCB hole center edge offset')
    p('pcb_hole_diameter', '0.34 cm', 'cm', 'M3 PCB clearance')
    p('insert_pilot_diameter', '0.42 cm', 'cm', '4.2 mm insert pilot; set to 0.50 cm for 5 mm insert')
    p('pcb_clearance', '0.5 cm', 'cm', '5 mm battery and connector clearance')
    p('tof_yaw', '25 deg', 'deg', 'Low ToF left/right yaw')

    root = design.rootComponent
    sketches = root.sketches
    feats = root.features
    xy = root.xYConstructionPlane

    # 1. Chassis master sketch
    sk = sketches.add(xy)
    sk.name = '01 Chassis master sketch'
    lines = sk.sketchCurves.sketchLines
    lines.addTwoPointRectangle(adsk.core.Point3D.create(-11, -9, 0), adsk.core.Point3D.create(11, 9, 0))
    # 2. Base body extrusion
    prof = sk.profiles.item(0)
    ext = feats.extrudeFeatures.addSimple(prof, adsk.core.ValueInput.createByString('floor_thickness'), adsk.fusion.FeatureOperations.NewBodyFeatureOperation)
    ext.name = '02 PLA chassis floor'

    # 3. Motor and wheel interface reference pockets (chronological construction features)
    skm = sketches.add(xy)
    skm.name = '03 Motor interface layout'
    for x in (-7.0, 7.0):
        lines.addTwoPointRectangle(adsk.core.Point3D.create(x-1.5, -6, 0), adsk.core.Point3D.create(x+1.5, 1, 0))
    # 4. Battery slide-in slot, centered, open through front/rear by a shallow pocket
    skb = sketches.add(xy)
    skb.name = '04 E326S battery slide slot'
    skb.sketchCurves.sketchLines.addTwoPointRectangle(adsk.core.Point3D.create(-7.5, -4.0, 0), adsk.core.Point3D.create(7.5, 4.0, 0))
    # pocket cut stops above the floor, leaving a 3 mm structural base
    if skb.profiles.count:
        cut = feats.extrudeFeatures.addSimple(skb.profiles.item(0), adsk.core.ValueInput.createByString('0.15 cm'), adsk.fusion.FeatureOperations.CutFeatureOperation)
        cut.name = '05 Battery pocket clearance'

    # 5. PCB bosses with parametric pilot holes
    skb2 = sketches.add(xy)
    skb2.name = '06 PCB boss datums (5 mm edge offsets)'
    for x in (-4.5, 4.5):
        for y in (-4.0, 4.0):
            skb2.sketchCurves.sketchCircles.addByCenterRadius(adsk.core.Point3D.create(x, y, 0), 0.75)
    if skb2.profiles.count:
        boss = feats.extrudeFeatures.addSimple(skb2.profiles.item(0), adsk.core.ValueInput.createByString('2.5 cm'), adsk.fusion.FeatureOperations.JoinFeatureOperation)
        boss.name = '07 Raised PCB brass-standoff bosses'

    # 6. Three ToF mounting datums
    skt = sketches.add(xy)
    skt.name = '08 Three VL53L0X front sensor datums'
    for x in (-5.5, 0, 5.5):
        skt.sketchCurves.sketchLines.addTwoPointRectangle(adsk.core.Point3D.create(x-0.6, 7.6, 0), adsk.core.Point3D.create(x+0.6, 8.8, 0))

    # 7. Four QRE corner mounting datums
    skq = sketches.add(xy)
    skq.name = '09 Four QRE1113 corner datums'
    for x in (-8.0, 8.0):
        for y in (-7.0, 7.0):
            skq.sketchCurves.sketchCircles.addByCenterRadius(adsk.core.Point3D.create(x, y, 0), 0.35)

    # 8. Passive front wedge reference
    skw = sketches.add(xy)
    skw.name = '10 Passive rounded front wedge'
    skw.sketchCurves.sketchLines.addTwoPointRectangle(adsk.core.Point3D.create(-10.5, 7.5, 0), adsk.core.Point3D.create(10.5, 9.0, 0))

    # Keep construction sketches visible in the timeline but hide their graphics.
    for s in sketches:
        s.isLightBulbOn = False
    app.activeViewport.fit()
    print('Created bottlesumo_ai with named timeline features and user parameters')
