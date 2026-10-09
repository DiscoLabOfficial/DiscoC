#pragma once

#include <cstdint>

// Enum for the base opcode byte of each instruction.
// Based on https://en.wikibooks.org/wiki/Super_NES_Programming/Super_FX_tutorial
enum class OpCode : uint8_t {
    STOP = 0x00,
    NOP  = 0x01,
    CACHE= 0x02,
    LSR  = 0x03, // Logical Shift Right
    ROL  = 0x04, // Rotate Left
    BRA  = 0x05, // Branch Always
    BGE  = 0x06, // Branch on >= 0
    BLT  = 0x07, // Branch on < 0
    BNE  = 0x08, // Branch on Not Equal
    BEQ  = 0x09, // Branch on Equal
    BPL  = 0x0A, // Branch on Plus
    BMI  = 0x0B, // Branch on Minus (Negative)
    BCC  = 0x0C, // Branch on Carry Clear
    BCS  = 0x0D, // Branch on Carry Set
    BVC  = 0x0E, // Branch on Overflow Clear
    BVS  = 0x0F, // Branch on Overflow Set

    // These opcodes have their argument register encoded in the lower nibble
    LOB = 0x9E, // Load low byte and zero-extend
    HIB = 0xC0, // Load high byte and zero-extend (S reflects byte bit 7)
    SWAP = 0x4D, // Exchange high/low bytes
    SEX = 0x95, // Sign-extend low byte
    TO   = 0x10, // Base for TO R0-R15 -> 0x10-0x1F
    STW_R = 0x30, // Base for STW (Rn) -> 0x30-0x3F, e.g. STW (R1) is 0x31
    ADD_R = 0x50, // Base for ADD Rn -> 0x50-0x5F
    JMP_R = 0x90, // Base for JMP Rn -> 0x90-0x9F

    // These are multi-byte opcodes where the instruction is a full byte
    IBT  = 0xA0, // Load Immediate Byte to Register. Full form: A_ R_ B_
    IWT  = 0xF0, // Load Immediate Word to Register. Full form: F_ R_ W_ W_
    
    // ALT Prefixes
    ALT1 = 0x3D,
    ALT2 = 0x3E,
    ALT3 = 0x3F, // ALT1 + ALT2

	COLOR_R = 0x4E,
    CMODE = 0x4E, // CMODE shares an opcode with COLOR, distinguished by ALT1
    RPIX  = 0x4C, // RPIX shares an opcode with PLOT, distinguished by ALT1
	GETC = 0xDF,
	ROMB = GETC,
	RAMB = GETC,
    GETB = 0xEF,
};

// Register Definitions
enum class Register : uint8_t {
    R0 = 0, R1_PIXEL_X = 1, R2_PIXEL_Y = 2, R3,
    R4_LMULT_LO, R5, R6_LMULT_HI, R7_MERGE_X,
    R8_MERGE_Y, R9, R10_SP, // R10 is now our chosen Stack Pointer
    R11_LINK,             // Return address from LINK instruction
    R12_LOOP_COUNT,
    R13_LOOP_ADDR,
    R14_LR,               // Link Register for BSR, and ROM address pointer
    R15_PC
};

// By convention, R0 is used for function return values.
constexpr Register RETURN_VALUE_REGISTER = Register::R0;

// By our new ABI, R10 is the stack pointer.
constexpr Register STACK_POINTER_REGISTER = Register::R10_SP;
