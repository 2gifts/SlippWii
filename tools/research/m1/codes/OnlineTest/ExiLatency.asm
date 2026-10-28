################################################################################
# Address: 0x80376a28 # HSD_PadRenewRawStatus right after PAD_Read call
################################################################################
# Slippi online on Wii, measurement build (M1). Every frame, at the hook Slippi's
# online code uses for its input exchange (Online/Core/TriggerSendInput.asm), do
# the same EXI traffic as command 0xB0 (during Versus matches only): a 32-byte write, then a 320-byte read
# (the online reply is 297 bytes). Time both with the timebase and send the
# previous frame's timings in the next write, for the Nintendont kernel to
# collect (kernel/SlippiOnlineTest.c). Command 0xF7 is unused by Slippi.
################################################################################
.include "Common/Common.s"

.set REG_DATA, 31
.set REG_BUF, 30
.set REG_T0, 29
.set REG_T1, 28
.set REG_T2, 27

backup

b CODE_START

DATA_BLRL:
blrl
.long 0         # 0x00 frame counter
.long 0         # 0x04 previous write ticks
.long 0         # 0x08 previous read ticks
.long 0         # 0x0C unused
.space 0x160    # 0x10 transfer buffer (aligned to 32 at run time)

CODE_START:
# Only during a Versus match, like Slippi's recording and online codes. At boot,
# Melee's memory card library is probing slot B; EXI attach/detach on slot B
# then crashed it (DSI in the CARD code, hardware test 2026-10-05).
getMinorMajor r3
cmpwi r3, SCENE_VERSUS_IN_GAME
bne EXIT

bl DATA_BLRL
mflr REG_DATA
addi REG_BUF, REG_DATA, 0x10 + 31
rlwinm REG_BUF, REG_BUF, 0, 0, 26

lwz r3, 0x0(REG_DATA)
addi r3, r3, 1
stw r3, 0x0(REG_DATA)

li r4, 0xF7
stb r4, 0x0(REG_BUF)
stw r3, 0x1(REG_BUF)
mftb REG_T0
stw REG_T0, 0x5(REG_BUF)
lwz r4, 0x4(REG_DATA)
stw r4, 0x9(REG_BUF)
lwz r4, 0x8(REG_DATA)
stw r4, 0xD(REG_BUF)

mr r3, REG_BUF
li r4, 32
li r5, CONST_ExiWrite
branchl r12, FN_EXITransferBuffer
mftb REG_T1

mr r3, REG_BUF
li r4, 0x140
li r5, CONST_ExiRead
branchl r12, FN_EXITransferBuffer
mftb REG_T2

sub r3, REG_T1, REG_T0
stw r3, 0x4(REG_DATA)
sub r3, REG_T2, REG_T1
stw r3, 0x8(REG_DATA)

EXIT:
restore
cmpwi r30, 0 # restore replaced instruction
