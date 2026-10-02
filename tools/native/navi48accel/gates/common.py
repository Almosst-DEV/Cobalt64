import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT = os.path.abspath(os.path.join(HERE, '../../ioaccel-layout'))
sys.path.insert(0, LAYOUT)
sys.path.insert(0, HERE)
from leaves import LEAVES, OSDEFINE_SLOTS, TRAMP, HAND, EXPECT_CLASSES, EXPECT_SLOTS, NEGATIVE_CLASS

def model():
    return json.load(open(os.path.join(LAYOUT, 'generated', 'model.json')))['classes']

def forwarded():
    return json.load(open(os.path.join(LAYOUT, 'generated', 'forwarded.json')))

def override_slots(leaf, parent, extra, C=None, fwd=None):
    """Expected DEFINED (overridden) slots of the leaf's vtable: OSDefine slots + the parent's pure slots + the parent's forwarded slots + extras."""
    C = C or model(); fwd = fwd or forwarded()
    s = set(OSDEFINE_SLOTS)
    s |= {r['i'] for r in C[parent]['rows'] if r['pure']}
    s |= set(fwd.get(parent, []))
    s |= set(extra)
    if leaf in TRAMP: s |= set(TRAMP[leaf][1])
    return sorted(s)

def ztv(name):
    return '__ZTV%d%s' % (len(name), name)
