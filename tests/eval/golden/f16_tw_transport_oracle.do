* P1-A3-REVORACLE-SCAFFOLD -- reverse-oracle harness, leg (2) transport identity
* + leg (3) metadata-capture scaffold (internal note P1-A3-WRITER-IMPL-brief.md Sec2.2/Sec2.3).
* "Stata as TRANSPORT, not SIGNER": round-trip the already-committed golden/
* f16_tw_fixture.dta through Stata's OWN use+save (no cobra writer exists yet --
* zero writer code exercised). Captures `describe, fullnames` for BOTH the
* original and the round-tripped file as the leg-3 truth-source, and saves the
* round-tripped .dta as the leg-2 artifact (compared by the engine's own
* datasignature on each side in properties.p1a3_transport_leg2_identity /
* p1a3_transport_leg3_metadata -- see tests/eval/properties.py).
clear
di "MARK_ORIG_USE"
use "f16_tw_fixture.dta", clear
di "MARK_ORIG_DESCRIBE"
describe, fullnames
list

di "MARK_ROUNDTRIP_SAVE"
save "f16_tw_fixture_roundtrip.dta", replace

di "MARK_ROUNDTRIP_USE"
use "f16_tw_fixture_roundtrip.dta", clear
di "MARK_ROUNDTRIP_DESCRIBE"
describe, fullnames
list

di "MARK_DONE"
exit, clear
