// iodp_vtable_static.h - GENERATED (0.0.613, #11 step 11h.2) from tools/native/ioaccel-layout/vtables/IOAccelDisplayPipe.tsv (sha256 prefix adc37edf76f1a879).
// __ZTV18IOAccelDisplayPipe (IOAcceleratorFamily2, tahoe 26.6.2 x86_64): 306 slots. level 1 = an IOAcceleratorFamily2 (System KC) address, slid by the
// family slide; level 0 = a kernel (Boot KC) address, slid by the kernel slide. tests/native_disp_test.cpp re-reads the TSV and compares every row.
// Regenerate: see the awk line in the 0.0.613 report; never edit by hand.
#pragma once
#include <stdint.h>
namespace n48disp {
struct VtStatic { uint8_t level; uint64_t addr; };
constexpr uint32_t kIodpSlots = 306u;
constexpr VtStatic kIodpStatic[kIodpSlots] = {
    { 1, 0x145cb166ull },   // 0 IOAccelDisplayPipe::~IOAccelDisplayPipe()
    { 1, 0x145cb170ull },   // 1 IOAccelDisplayPipe::~IOAccelDisplayPipe()
    { 0, 0xffffff8000a1b3d0ull },   // 2 OSObject::release(int) const
    { 0, 0xffffff8000a1b3f0ull },   // 3 OSObject::getRetainCount() const
    { 0, 0xffffff8000a1b400ull },   // 4 OSObject::retain() const
    { 0, 0xffffff8000a1b410ull },   // 5 OSObject::release() const
    { 0, 0xffffff8000a1b420ull },   // 6 OSObject::serialize(OSSerialize*) const
    { 1, 0x145cb1b0ull },   // 7 IOAccelDisplayPipe::getMetaClass() const
    { 0, 0xffffff8000a19690ull },   // 8 OSMetaClassBase::isEqualTo(OSMetaClassBase const*) const
    { 0, 0xffffff8000a1b5a0ull },   // 9 OSObject::taggedRetain(void const*) const
    { 0, 0xffffff8000a1b610ull },   // 10 OSObject::taggedRelease(void const*) const
    { 0, 0xffffff8000a1b630ull },   // 11 OSObject::taggedRelease(void const*, int) const
    { 0, 0xffffff8000a67120ull },   // 12 IOService::Dispatch(IORPC)
    { 0, 0xffffff8000bb3bc0ull },   // 13 OSMetaClassBase::_RESERVEDOSMetaClassBase4()
    { 0, 0xffffff8000bb3bf0ull },   // 14 OSMetaClassBase::_RESERVEDOSMetaClassBase5()
    { 0, 0xffffff8000bb3c20ull },   // 15 OSMetaClassBase::_RESERVEDOSMetaClassBase6()
    { 0, 0xffffff8000bb3c50ull },   // 16 OSMetaClassBase::_RESERVEDOSMetaClassBase7()
    { 0, 0xffffff8000a1b6d0ull },   // 17 OSObject::init()
    { 1, 0x145cb9e8ull },   // 18 IOAccelDisplayPipe::free()
    { 0, 0xffffff8000bb3ec0ull },   // 19 OSObject::_RESERVEDOSObject0()
    { 0, 0xffffff8000bb3ee0ull },   // 20 OSObject::_RESERVEDOSObject1()
    { 0, 0xffffff8000bb3f00ull },   // 21 OSObject::_RESERVEDOSObject2()
    { 0, 0xffffff8000bb3f20ull },   // 22 OSObject::_RESERVEDOSObject3()
    { 0, 0xffffff8000bb3f40ull },   // 23 OSObject::_RESERVEDOSObject4()
    { 0, 0xffffff8000bb3f60ull },   // 24 OSObject::_RESERVEDOSObject5()
    { 0, 0xffffff8000bb3f80ull },   // 25 OSObject::_RESERVEDOSObject6()
    { 0, 0xffffff8000bb3fa0ull },   // 26 OSObject::_RESERVEDOSObject7()
    { 0, 0xffffff8000bb3fc0ull },   // 27 OSObject::_RESERVEDOSObject8()
    { 0, 0xffffff8000bb3fe0ull },   // 28 OSObject::_RESERVEDOSObject9()
    { 0, 0xffffff8000bb4000ull },   // 29 OSObject::_RESERVEDOSObject10()
    { 0, 0xffffff8000bb4020ull },   // 30 OSObject::_RESERVEDOSObject11()
    { 0, 0xffffff8000bb4040ull },   // 31 OSObject::_RESERVEDOSObject12()
    { 0, 0xffffff8000bb4060ull },   // 32 OSObject::_RESERVEDOSObject13()
    { 0, 0xffffff8000bb4080ull },   // 33 OSObject::_RESERVEDOSObject14()
    { 0, 0xffffff8000bb40a0ull },   // 34 OSObject::_RESERVEDOSObject15()
    { 0, 0xffffff8000a8b180ull },   // 35 IORegistryEntry::copyProperty(char const*, IORegistryPlane const*, unsigned int) const
    { 0, 0xffffff8000a8b2e0ull },   // 36 IORegistryEntry::copyProperty(OSString const*, IORegistryPlane const*, unsigned int) const
    { 0, 0xffffff8000a8b440ull },   // 37 IORegistryEntry::copyProperty(OSSymbol const*, IORegistryPlane const*, unsigned int) const
    { 0, 0xffffff8000a8b5a0ull },   // 38 IORegistryEntry::copyParentEntry(IORegistryPlane const*) const
    { 0, 0xffffff8000a8b610ull },   // 39 IORegistryEntry::copyChildEntry(IORegistryPlane const*) const
    { 0, 0xffffff8000a8b680ull },   // 40 IORegistryEntry::runPropertyAction(int (*)(OSObject*, void*, void*, void*, void*), OSObject*, void*, void*, void*, void*)
    { 0, 0xffffff8000bb55d0ull },   // 41 IORegistryEntry::_RESERVEDIORegistryEntry0()
    { 0, 0xffffff8000bb55f0ull },   // 42 IORegistryEntry::_RESERVEDIORegistryEntry1()
    { 0, 0xffffff8000bb5610ull },   // 43 IORegistryEntry::_RESERVEDIORegistryEntry2()
    { 0, 0xffffff8000bb5630ull },   // 44 IORegistryEntry::_RESERVEDIORegistryEntry3()
    { 0, 0xffffff8000bb5650ull },   // 45 IORegistryEntry::_RESERVEDIORegistryEntry4()
    { 0, 0xffffff8000bb5670ull },   // 46 IORegistryEntry::_RESERVEDIORegistryEntry5()
    { 0, 0xffffff8000bb5690ull },   // 47 IORegistryEntry::_RESERVEDIORegistryEntry6()
    { 0, 0xffffff8000bb56b0ull },   // 48 IORegistryEntry::_RESERVEDIORegistryEntry7()
    { 0, 0xffffff8000bb56d0ull },   // 49 IORegistryEntry::_RESERVEDIORegistryEntry8()
    { 0, 0xffffff8000bb56f0ull },   // 50 IORegistryEntry::_RESERVEDIORegistryEntry9()
    { 0, 0xffffff8000bb5710ull },   // 51 IORegistryEntry::_RESERVEDIORegistryEntry10()
    { 0, 0xffffff8000bb5730ull },   // 52 IORegistryEntry::_RESERVEDIORegistryEntry11()
    { 0, 0xffffff8000bb5750ull },   // 53 IORegistryEntry::_RESERVEDIORegistryEntry12()
    { 0, 0xffffff8000bb5770ull },   // 54 IORegistryEntry::_RESERVEDIORegistryEntry13()
    { 0, 0xffffff8000bb5790ull },   // 55 IORegistryEntry::_RESERVEDIORegistryEntry14()
    { 0, 0xffffff8000bb57b0ull },   // 56 IORegistryEntry::_RESERVEDIORegistryEntry15()
    { 0, 0xffffff8000bb57d0ull },   // 57 IORegistryEntry::_RESERVEDIORegistryEntry16()
    { 0, 0xffffff8000bb57f0ull },   // 58 IORegistryEntry::_RESERVEDIORegistryEntry17()
    { 0, 0xffffff8000bb5810ull },   // 59 IORegistryEntry::_RESERVEDIORegistryEntry18()
    { 0, 0xffffff8000bb5830ull },   // 60 IORegistryEntry::_RESERVEDIORegistryEntry19()
    { 0, 0xffffff8000bb5850ull },   // 61 IORegistryEntry::_RESERVEDIORegistryEntry20()
    { 0, 0xffffff8000bb5870ull },   // 62 IORegistryEntry::_RESERVEDIORegistryEntry21()
    { 0, 0xffffff8000bb5890ull },   // 63 IORegistryEntry::_RESERVEDIORegistryEntry22()
    { 0, 0xffffff8000bb58b0ull },   // 64 IORegistryEntry::_RESERVEDIORegistryEntry23()
    { 0, 0xffffff8000bb58d0ull },   // 65 IORegistryEntry::_RESERVEDIORegistryEntry24()
    { 0, 0xffffff8000bb58f0ull },   // 66 IORegistryEntry::_RESERVEDIORegistryEntry25()
    { 0, 0xffffff8000bb5910ull },   // 67 IORegistryEntry::_RESERVEDIORegistryEntry26()
    { 0, 0xffffff8000bb5930ull },   // 68 IORegistryEntry::_RESERVEDIORegistryEntry27()
    { 0, 0xffffff8000bb5950ull },   // 69 IORegistryEntry::_RESERVEDIORegistryEntry28()
    { 0, 0xffffff8000bb5970ull },   // 70 IORegistryEntry::_RESERVEDIORegistryEntry29()
    { 0, 0xffffff8000bb5990ull },   // 71 IORegistryEntry::_RESERVEDIORegistryEntry30()
    { 0, 0xffffff8000bb59b0ull },   // 72 IORegistryEntry::_RESERVEDIORegistryEntry31()
    { 0, 0xffffff8000a91950ull },   // 73 IOService::init(OSDictionary*)
    { 0, 0xffffff8000a8b920ull },   // 74 IORegistryEntry::setPropertyTable(OSDictionary*)
    { 0, 0xffffff8000a8b9c0ull },   // 75 IORegistryEntry::setProperty(OSSymbol const*, OSObject*)
    { 0, 0xffffff8000a8bae0ull },   // 76 IORegistryEntry::setProperty(OSString const*, OSObject*)
    { 0, 0xffffff8000a8bb30ull },   // 77 IORegistryEntry::setProperty(char const*, OSObject*)
    { 0, 0xffffff8000a8bb80ull },   // 78 IORegistryEntry::setProperty(char const*, char const*)
    { 0, 0xffffff8000a8bbf0ull },   // 79 IORegistryEntry::setProperty(char const*, bool)
    { 0, 0xffffff8000a8bc60ull },   // 80 IORegistryEntry::setProperty(char const*, unsigned long long, unsigned int)
    { 0, 0xffffff8000a8bd00ull },   // 81 IORegistryEntry::setProperty(char const*, void*, unsigned int)
    { 0, 0xffffff8000a8bda0ull },   // 82 IORegistryEntry::removeProperty(OSSymbol const*)
    { 0, 0xffffff8000a8be20ull },   // 83 IORegistryEntry::removeProperty(OSString const*)
    { 0, 0xffffff8000a8be60ull },   // 84 IORegistryEntry::removeProperty(char const*)
    { 0, 0xffffff8000a8bea0ull },   // 85 IORegistryEntry::getProperty(OSSymbol const*) const
    { 0, 0xffffff8000a8bf20ull },   // 86 IORegistryEntry::getProperty(OSString const*) const
    { 0, 0xffffff8000a8bf60ull },   // 87 IORegistryEntry::getProperty(char const*) const
    { 0, 0xffffff8000a8bfa0ull },   // 88 IORegistryEntry::getProperty(OSSymbol const*, IORegistryPlane const*, unsigned int) const
    { 0, 0xffffff8000a8c100ull },   // 89 IORegistryEntry::getProperty(OSString const*, IORegistryPlane const*, unsigned int) const
    { 0, 0xffffff8000a8c260ull },   // 90 IORegistryEntry::getProperty(char const*, IORegistryPlane const*, unsigned int) const
    { 0, 0xffffff8000a8c3c0ull },   // 91 IORegistryEntry::copyProperty(OSSymbol const*) const
    { 0, 0xffffff8000a8c450ull },   // 92 IORegistryEntry::copyProperty(OSString const*) const
    { 0, 0xffffff8000a8c4e0ull },   // 93 IORegistryEntry::copyProperty(char const*) const
    { 0, 0xffffff8000a8c570ull },   // 94 IORegistryEntry::dictionaryWithProperties() const
    { 0, 0xffffff8000a919b0ull },   // 95 IOService::serializeProperties(OSSerialize*) const
    { 0, 0xffffff8000a8c6d0ull },   // 96 IORegistryEntry::setProperties(OSObject*)
    { 0, 0xffffff8000a8c6e0ull },   // 97 IORegistryEntry::getParentIterator(IORegistryPlane const*) const
    { 0, 0xffffff8000a8c810ull },   // 98 IORegistryEntry::applyToParents(void (*)(IORegistryEntry*, void*), void*, IORegistryPlane const*) const
    { 0, 0xffffff8000a8c910ull },   // 99 IORegistryEntry::getParentEntry(IORegistryPlane const*) const
    { 0, 0xffffff8000a8c940ull },   // 100 IORegistryEntry::getChildIterator(IORegistryPlane const*) const
    { 0, 0xffffff8000a8ca70ull },   // 101 IORegistryEntry::applyToChildren(void (*)(IORegistryEntry*, void*), void*, IORegistryPlane const*) const
    { 0, 0xffffff8000a8cb70ull },   // 102 IORegistryEntry::getChildEntry(IORegistryPlane const*) const
    { 0, 0xffffff8000a8cba0ull },   // 103 IORegistryEntry::isChild(IORegistryEntry*, IORegistryPlane const*, bool) const
    { 0, 0xffffff8000a8ccd0ull },   // 104 IORegistryEntry::isParent(IORegistryEntry*, IORegistryPlane const*, bool) const
    { 0, 0xffffff8000a8ce00ull },   // 105 IORegistryEntry::inPlane(IORegistryPlane const*) const
    { 0, 0xffffff8000a8cf30ull },   // 106 IORegistryEntry::getDepth(IORegistryPlane const*) const
    { 0, 0xffffff8000a8d030ull },   // 107 IORegistryEntry::attachToParent(IORegistryEntry*, IORegistryPlane const*)
    { 0, 0xffffff8000a8d320ull },   // 108 IORegistryEntry::detachFromParent(IORegistryEntry*, IORegistryPlane const*)
    { 0, 0xffffff8000a8d520ull },   // 109 IORegistryEntry::attachToChild(IORegistryEntry*, IORegistryPlane const*)
    { 0, 0xffffff8000a8d630ull },   // 110 IORegistryEntry::detachFromChild(IORegistryEntry*, IORegistryPlane const*)
    { 0, 0xffffff8000a8d820ull },   // 111 IORegistryEntry::detachAbove(IORegistryPlane const*)
    { 0, 0xffffff8000a8d8a0ull },   // 112 IORegistryEntry::detachAll(IORegistryPlane const*)
    { 0, 0xffffff8000a8da30ull },   // 113 IORegistryEntry::getName(IORegistryPlane const*) const
    { 0, 0xffffff8000a8dad0ull },   // 114 IORegistryEntry::copyName(IORegistryPlane const*) const
    { 0, 0xffffff8000a8db80ull },   // 115 IORegistryEntry::compareNames(OSObject*, OSString**) const
    { 0, 0xffffff8000a8dcd0ull },   // 116 IORegistryEntry::compareName(OSString*, OSString**) const
    { 0, 0xffffff8000a8dd30ull },   // 117 IORegistryEntry::setName(OSSymbol const*, IORegistryPlane const*)
    { 0, 0xffffff8000a8de10ull },   // 118 IORegistryEntry::setName(char const*, IORegistryPlane const*)
    { 0, 0xffffff8000a8de60ull },   // 119 IORegistryEntry::getLocation(IORegistryPlane const*) const
    { 0, 0xffffff8000a8dea0ull },   // 120 IORegistryEntry::copyLocation(IORegistryPlane const*) const
    { 0, 0xffffff8000a8df20ull },   // 121 IORegistryEntry::setLocation(OSSymbol const*, IORegistryPlane const*)
    { 0, 0xffffff8000a8df90ull },   // 122 IORegistryEntry::setLocation(char const*, IORegistryPlane const*)
    { 0, 0xffffff8000a8dfe0ull },   // 123 IORegistryEntry::getPath(char*, int*, IORegistryPlane const*) const
    { 0, 0xffffff8000a8e590ull },   // 124 IORegistryEntry::getPathComponent(char*, int*, IORegistryPlane const*) const
    { 0, 0xffffff8000a8e680ull },   // 125 IORegistryEntry::childFromPath(char const*, IORegistryPlane const*, char*, int*)
    { 0, 0xffffff8000a919c0ull },   // 126 IOService::init(IORegistryEntry*, IORegistryPlane const*)
    { 0, 0xffffff8000a91a20ull },   // 127 IOService::requestTerminate(IOService*, unsigned int)
    { 0, 0xffffff8000a91a80ull },   // 128 IOService::willTerminate(IOService*, unsigned int)
    { 0, 0xffffff8000a91ae0ull },   // 129 IOService::didTerminate(IOService*, unsigned int, bool*)
    { 0, 0xffffff8000abb1e0ull },   // 130 IOService::nextIdleTimeout(unsigned long long, unsigned long long, unsigned int)
    { 0, 0xffffff8000abd310ull },   // 131 IOService::systemWillShutdown(unsigned int)
    { 0, 0xffffff8000a91b90ull },   // 132 IOService::copyClientWithCategory(OSSymbol const*)
    { 0, 0xffffff8000a91c70ull },   // 133 IOService::configureReport(IOReportChannelList*, unsigned int, void*, void*)
    { 0, 0xffffff8000a91ea0ull },   // 134 IOService::updateReport(IOReportChannelList*, unsigned int, void*, void*)
    { 0, 0xffffff8000bb5a00ull },   // 135 IOService::_RESERVEDIOService2()
    { 0, 0xffffff8000bb5a20ull },   // 136 IOService::_RESERVEDIOService3()
    { 0, 0xffffff8000bb5a40ull },   // 137 IOService::_RESERVEDIOService4()
    { 0, 0xffffff8000bb5a60ull },   // 138 IOService::_RESERVEDIOService5()
    { 0, 0xffffff8000bb5a80ull },   // 139 IOService::_RESERVEDIOService6()
    { 0, 0xffffff8000bb5aa0ull },   // 140 IOService::_RESERVEDIOService7()
    { 0, 0xffffff8000bb5ac0ull },   // 141 IOService::_RESERVEDIOService8()
    { 0, 0xffffff8000bb5ae0ull },   // 142 IOService::_RESERVEDIOService9()
    { 0, 0xffffff8000bb5b00ull },   // 143 IOService::_RESERVEDIOService10()
    { 0, 0xffffff8000bb5b20ull },   // 144 IOService::_RESERVEDIOService11()
    { 0, 0xffffff8000bb5b40ull },   // 145 IOService::_RESERVEDIOService12()
    { 0, 0xffffff8000bb5b60ull },   // 146 IOService::_RESERVEDIOService13()
    { 0, 0xffffff8000bb5b80ull },   // 147 IOService::_RESERVEDIOService14()
    { 0, 0xffffff8000bb5ba0ull },   // 148 IOService::_RESERVEDIOService15()
    { 0, 0xffffff8000bb5bc0ull },   // 149 IOService::_RESERVEDIOService16()
    { 0, 0xffffff8000bb5be0ull },   // 150 IOService::_RESERVEDIOService17()
    { 0, 0xffffff8000bb5c00ull },   // 151 IOService::_RESERVEDIOService18()
    { 0, 0xffffff8000bb5c20ull },   // 152 IOService::_RESERVEDIOService19()
    { 0, 0xffffff8000bb5c40ull },   // 153 IOService::_RESERVEDIOService20()
    { 0, 0xffffff8000bb5c60ull },   // 154 IOService::_RESERVEDIOService21()
    { 0, 0xffffff8000bb5c80ull },   // 155 IOService::_RESERVEDIOService22()
    { 0, 0xffffff8000bb5ca0ull },   // 156 IOService::_RESERVEDIOService23()
    { 0, 0xffffff8000bb5cc0ull },   // 157 IOService::_RESERVEDIOService24()
    { 0, 0xffffff8000bb5ce0ull },   // 158 IOService::_RESERVEDIOService25()
    { 0, 0xffffff8000bb5d00ull },   // 159 IOService::_RESERVEDIOService26()
    { 0, 0xffffff8000bb5d20ull },   // 160 IOService::_RESERVEDIOService27()
    { 0, 0xffffff8000bb5d40ull },   // 161 IOService::_RESERVEDIOService28()
    { 0, 0xffffff8000bb5d60ull },   // 162 IOService::_RESERVEDIOService29()
    { 0, 0xffffff8000bb5d80ull },   // 163 IOService::_RESERVEDIOService30()
    { 0, 0xffffff8000bb5da0ull },   // 164 IOService::_RESERVEDIOService31()
    { 0, 0xffffff8000bb5dc0ull },   // 165 IOService::_RESERVEDIOService32()
    { 0, 0xffffff8000bb5de0ull },   // 166 IOService::_RESERVEDIOService33()
    { 0, 0xffffff8000bb5e00ull },   // 167 IOService::_RESERVEDIOService34()
    { 0, 0xffffff8000bb5e20ull },   // 168 IOService::_RESERVEDIOService35()
    { 0, 0xffffff8000bb5e40ull },   // 169 IOService::_RESERVEDIOService36()
    { 0, 0xffffff8000bb5e60ull },   // 170 IOService::_RESERVEDIOService37()
    { 0, 0xffffff8000bb5e80ull },   // 171 IOService::_RESERVEDIOService38()
    { 0, 0xffffff8000bb5ea0ull },   // 172 IOService::_RESERVEDIOService39()
    { 0, 0xffffff8000bb5ec0ull },   // 173 IOService::_RESERVEDIOService40()
    { 0, 0xffffff8000bb5ee0ull },   // 174 IOService::_RESERVEDIOService41()
    { 0, 0xffffff8000bb5f00ull },   // 175 IOService::_RESERVEDIOService42()
    { 0, 0xffffff8000bb5f20ull },   // 176 IOService::_RESERVEDIOService43()
    { 0, 0xffffff8000bb5f40ull },   // 177 IOService::_RESERVEDIOService44()
    { 0, 0xffffff8000bb5f60ull },   // 178 IOService::_RESERVEDIOService45()
    { 0, 0xffffff8000bb5f80ull },   // 179 IOService::_RESERVEDIOService46()
    { 0, 0xffffff8000bb5fa0ull },   // 180 IOService::_RESERVEDIOService47()
    { 0, 0xffffff8000a920b0ull },   // 181 IOService::getState() const
    { 0, 0xffffff8000a920c0ull },   // 182 IOService::registerService(unsigned int)
    { 0, 0xffffff8000a922c0ull },   // 183 IOService::probe(IOService*, int*)
    { 0, 0xffffff8000a922d0ull },   // 184 IOService::start(IOService*)
    { 0, 0xffffff8000a922e0ull },   // 185 IOService::stop(IOService*)
    { 0, 0xffffff8000a92310ull },   // 186 IOService::open(IOService*, unsigned int, void*)
    { 0, 0xffffff8000a92430ull },   // 187 IOService::close(IOService*, unsigned int)
    { 0, 0xffffff8000a92540ull },   // 188 IOService::isOpen(IOService const*) const
    { 0, 0xffffff8000a92580ull },   // 189 IOService::handleOpen(IOService*, unsigned int, void*)
    { 0, 0xffffff8000a925e0ull },   // 190 IOService::handleClose(IOService*, unsigned int)
    { 0, 0xffffff8000a92600ull },   // 191 IOService::handleIsOpen(IOService const*) const
    { 0, 0xffffff8000a92620ull },   // 192 IOService::terminate(unsigned int)
    { 0, 0xffffff8000a92630ull },   // 193 IOService::finalize(unsigned int)
    { 0, 0xffffff8000a92840ull },   // 194 IOService::lockForArbitration(bool)
    { 0, 0xffffff8000a92c60ull },   // 195 IOService::unlockForArbitration()
    { 0, 0xffffff8000a92e70ull },   // 196 IOService::terminateClient(IOService*, unsigned int)
    { 0, 0xffffff8000a92ec0ull },   // 197 IOService::getBusyState()
    { 0, 0xffffff8000a92ed0ull },   // 198 IOService::adjustBusy(int)
    { 0, 0xffffff8000a92f10ull },   // 199 IOService::matchPropertyTable(OSDictionary*, int*)
    { 0, 0xffffff8000a92f30ull },   // 200 IOService::matchPropertyTable(OSDictionary*)
    { 0, 0xffffff8000a92f40ull },   // 201 IOService::matchLocation(IOService*)
    { 0, 0xffffff8000a92f80ull },   // 202 IOService::addNeededResource(char const*)
    { 0, 0xffffff8000a930c0ull },   // 203 IOService::compareProperty(OSDictionary*, char const*)
    { 0, 0xffffff8000a93130ull },   // 204 IOService::compareProperty(OSDictionary*, OSString const*)
    { 0, 0xffffff8000a931a0ull },   // 205 IOService::compareProperties(OSDictionary*, OSCollection*)
    { 0, 0xffffff8000a93290ull },   // 206 IOService::attach(IOService*)
    { 0, 0xffffff8000a934e0ull },   // 207 IOService::detach(IOService*)
    { 0, 0xffffff8000a93880ull },   // 208 IOService::getProvider() const
    { 0, 0xffffff8000a938d0ull },   // 209 IOService::getWorkLoop() const
    { 0, 0xffffff8000a93900ull },   // 210 IOService::getProviderIterator() const
    { 0, 0xffffff8000a93920ull },   // 211 IOService::getOpenProviderIterator() const
    { 0, 0xffffff8000a93a10ull },   // 212 IOService::getClient() const
    { 0, 0xffffff8000a93a30ull },   // 213 IOService::getClientIterator() const
    { 0, 0xffffff8000a93a50ull },   // 214 IOService::getOpenClientIterator() const
    { 0, 0xffffff8000a93b40ull },   // 215 IOService::callPlatformFunction(OSSymbol const*, bool, void*, void*, void*, void*)
    { 0, 0xffffff8000a93c60ull },   // 216 IOService::callPlatformFunction(char const*, bool, void*, void*, void*, void*)
    { 0, 0xffffff8000a93ce0ull },   // 217 IOService::getResources()
    { 0, 0xffffff8000a93cf0ull },   // 218 IOService::getDeviceMemoryCount()
    { 0, 0xffffff8000a93d50ull },   // 219 IOService::getDeviceMemoryWithIndex(unsigned int)
    { 0, 0xffffff8000a93dc0ull },   // 220 IOService::mapDeviceMemoryWithIndex(unsigned int, unsigned int)
    { 0, 0xffffff8000a93e00ull },   // 221 IOService::getDeviceMemory()
    { 0, 0xffffff8000a93e50ull },   // 222 IOService::setDeviceMemory(OSArray*)
    { 0, 0xffffff8000a93e70ull },   // 223 IOService::registerInterrupt(int, OSObject*, void (*)(OSObject*, void*, IOService*, int), void*)
    { 0, 0xffffff8000a93f30ull },   // 224 IOService::unregisterInterrupt(int)
    { 0, 0xffffff8000a93fc0ull },   // 225 IOService::getInterruptType(int, int*)
    { 0, 0xffffff8000a94060ull },   // 226 IOService::enableInterrupt(int)
    { 0, 0xffffff8000a940d0ull },   // 227 IOService::disableInterrupt(int)
    { 0, 0xffffff8000a94140ull },   // 228 IOService::causeInterrupt(int)
    { 0, 0xffffff8000a941b0ull },   // 229 IOService::requestProbe(unsigned int)
    { 0, 0xffffff8000a941c0ull },   // 230 IOService::message(unsigned int, IOService*, void*)
    { 0, 0xffffff8000a941d0ull },   // 231 IOService::messageClient(unsigned int, OSObject*, void*, unsigned long)
    { 0, 0xffffff8000a944a0ull },   // 232 IOService::messageClients(unsigned int, void*, unsigned long)
    { 0, 0xffffff8000a944f0ull },   // 233 IOService::registerInterest(OSSymbol const*, int (*)(void*, void*, unsigned int, IOService*, void*, unsigned long), void*, void*)
    { 0, 0xffffff8000a945d0ull },   // 234 IOService::applyToProviders(void (*)(IOService*, void*), void*)
    { 0, 0xffffff8000a945f0ull },   // 235 IOService::applyToClients(void (*)(IOService*, void*), void*)
    { 0, 0xffffff8000a94610ull },   // 236 IOService::applyToInterested(OSSymbol const*, void (*)(OSObject*, void*), void*)
    { 0, 0xffffff8000a94660ull },   // 237 IOService::acknowledgeNotification(void*, unsigned int)
    { 0, 0xffffff8000a94670ull },   // 238 IOService::newUserClient(task*, void*, unsigned int, OSDictionary*, IOUserClient**)
    { 0, 0xffffff8000a94880ull },   // 239 IOService::newUserClient(task*, void*, unsigned int, IOUserClient**)
    { 0, 0xffffff8000a94890ull },   // 240 IOService::stringFromReturn(int)
    { 0, 0xffffff8000a94920ull },   // 241 IOService::errnoFromReturn(int)
    { 0, 0xffffff8000aaa7c0ull },   // 242 IOService::PMinit()
    { 0, 0xffffff8000ab8890ull },   // 243 IOService::PMstop()
    { 0, 0xffffff8000ab8870ull },   // 244 IOService::joinPMtree(IOService*)
    { 0, 0xffffff8000ab9860ull },   // 245 IOService::registerPowerDriver(IOService*, IOPMPowerState*, unsigned long)
    { 0, 0xffffff8000aba260ull },   // 246 IOService::requestPowerDomainState(unsigned long, IOPowerConnection*, unsigned long)
    { 0, 0xffffff8000abafb0ull },   // 247 IOService::activityTickle(unsigned long, unsigned long)
    { 0, 0xffffff8000abb220ull },   // 248 IOService::setAggressiveness(unsigned long, unsigned long)
    { 0, 0xffffff8000abb230ull },   // 249 IOService::getAggressiveness(unsigned long, unsigned long*)
    { 0, 0xffffff8000ab8bb0ull },   // 250 IOService::addPowerChild(IOService*)
    { 0, 0xffffff8000ab96b0ull },   // 251 IOService::removePowerChild(IOPowerConnection*)
    { 0, 0xffffff8000abb050ull },   // 252 IOService::setIdleTimerPeriod(unsigned long)
    { 0, 0xffffff8000abd140ull },   // 253 IOService::setPowerState(unsigned long, IOService*)
    { 0, 0xffffff8000abd1b0ull },   // 254 IOService::maxCapabilityForDomainState(unsigned long)
    { 0, 0xffffff8000abd210ull },   // 255 IOService::initialPowerStateForDomainState(unsigned long)
    { 0, 0xffffff8000abd280ull },   // 256 IOService::powerStateForDomainState(unsigned long)
    { 0, 0xffffff8000abd2e0ull },   // 257 IOService::powerStateWillChangeTo(unsigned long, unsigned long, IOService*)
    { 0, 0xffffff8000abd2f0ull },   // 258 IOService::powerStateDidChangeTo(unsigned long, unsigned long, IOService*)
    { 0, 0xffffff8000abbc40ull },   // 259 IOService::askChangeDown(unsigned long)
    { 0, 0xffffff8000abbc50ull },   // 260 IOService::tellChangeDown(unsigned long)
    { 0, 0xffffff8000abbfa0ull },   // 261 IOService::tellNoChangeDown(unsigned long)
    { 0, 0xffffff8000abc5c0ull },   // 262 IOService::tellChangeUp(unsigned long)
    { 0, 0xffffff8000abc700ull },   // 263 IOService::allowPowerChange(unsigned long)
    { 0, 0xffffff8000abc780ull },   // 264 IOService::cancelPowerChange(unsigned long)
    { 0, 0xffffff8000abd300ull },   // 265 IOService::powerChangeDone(unsigned long)
    { 1, 0x145cb2acull },   // 266 IOAccelDisplayPipe::init(IOGraphicsAccelerator2*, IOAccelDisplayMachine*, IOFramebuffer*, unsigned int)
    { 1, 0x145cc264ull },   // 267 IOAccelDisplayPipe::initFramebufferResource(unsigned int, IOAccelResource2*)
    { 1, 0x145cc292ull },   // 268 IOAccelDisplayPipe::destroyFramebufferResource(unsigned int, IOAccelResource2*)
    { 1, 0x145cc602ull },   // 269 IOAccelDisplayPipe::displayModeWillChange()
    { 1, 0x145cc636ull },   // 270 IOAccelDisplayPipe::displayModeDidChange()
    { 1, 0x145cf514ull },   // 271 IOAccelDisplayPipe::enableVBLInterrupt()
    { 1, 0x145cf552ull },   // 272 IOAccelDisplayPipe::disableVBLInterrupt()
    { 1, 0x145cf588ull },   // 273 IOAccelDisplayPipe::enableTransactionInterrupt()
    { 1, 0x145cf5a8ull },   // 274 IOAccelDisplayPipe::disableTransactionInterrupt()
    { 1, 0x145cd148ull },   // 275 IOAccelDisplayPipe::newDisplayPipeTransaction()
    { 1, 0x145cfcecull },   // 276 IOAccelDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2*)
    { 1, 0x145cfcf4ull },   // 277 IOAccelDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2*)
    { 1, 0x145cfd00ull },   // 278 IOAccelDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2*)
    { 1, 0x145cf0e0ull },   // 279 IOAccelDisplayPipe::submitTransaction(IOAccelDisplayPipeTransaction2*)
    { 1, 0x145cc2c0ull },   // 280 IOAccelDisplayPipe::getDisplayModePipeScalerSetup(IOAccelDisplayPipeScaler*)
    { 1, 0x145cbfe0ull },   // 281 IOAccelDisplayPipe::createWorkLoop()
    { 1, 0x145cc2ccull },   // 282 IOAccelDisplayPipe::copyCapabilities()
    { 1, 0x145cf5c8ull },   // 283 IOAccelDisplayPipe::beginTransaction(IOAccelEvent*)
    { 1, 0x145cf5ceull },   // 284 IOAccelDisplayPipe::signalTransactionComplete(IOAccelEvent*)
    { 1, 0x145cc0f6ull },   // 285 IOAccelDisplayPipe::framebufferTerminated()
    { 1, 0x145cc860ull },   // 286 IOAccelDisplayPipe::wsaaEnterDefer(int)
    { 1, 0x145cc8f4ull },   // 287 IOAccelDisplayPipe::wsaaWillExitDefer(int)
    { 1, 0x145ccb6eull },   // 288 IOAccelDisplayPipe::wsaaDidExitDefer(int)
    { 1, 0x145cc868ull },   // 289 IOAccelDisplayPipe::wsaaWillEnterDefer(int)
    { 1, 0x145cc8e4ull },   // 290 IOAccelDisplayPipe::wsaaDidEnterDefer(int)
    { 1, 0x145cc098ull },   // 291 IOAccelDisplayPipe::willPowerOff()
    { 1, 0x145cc0d8ull },   // 292 IOAccelDisplayPipe::didPowerOn()
    { 1, 0x145cf5d4ull },   // 293 IOAccelDisplayPipe::logTransactionTimeoutDiagnosisReport()
    { 1, 0x145d0544ull },   // 294 IOAccelDisplayPipe::getStampIndex() const
    { 1, 0x145d0858ull },   // 295 IOAccelDisplayPipe::triage(char**, unsigned long long*)
    { 1, 0x145d0b50ull },   // 296 IOAccelDisplayPipe::_RESERVEDIOAccelDisplayPipe1()
    { 1, 0x145d0b66ull },   // 297 IOAccelDisplayPipe::_RESERVEDIOAccelDisplayPipe2()
    { 1, 0x145d0b7cull },   // 298 IOAccelDisplayPipe::_RESERVEDIOAccelDisplayPipe3()
    { 1, 0x145d0b92ull },   // 299 IOAccelDisplayPipe::_RESERVEDIOAccelDisplayPipe4()
    { 1, 0x145d0ba8ull },   // 300 IOAccelDisplayPipe::_RESERVEDIOAccelDisplayPipe5()
    { 1, 0x145d0bbeull },   // 301 IOAccelDisplayPipe::_RESERVEDIOAccelDisplayPipe6()
    { 1, 0x145d0bd4ull },   // 302 IOAccelDisplayPipe::_RESERVEDIOAccelDisplayPipe7()
    { 1, 0x145cc09eull },   // 303 IOAccelDisplayPipe::framebuffer_will_power_off()
    { 1, 0x145cc0deull },   // 304 IOAccelDisplayPipe::framebuffer_did_power_on()
    { 1, 0x145cc0fcull },   // 305 IOAccelDisplayPipe::framebuffer_terminated()
};
} // namespace n48disp
