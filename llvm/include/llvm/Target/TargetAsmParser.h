//===-- llvm/Target/TargetAsmParser.h - Target Assembly Parser --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGET_TARGETPARSER_H
#define LLVM_TARGET_TARGETPARSER_H

namespace llvm {
class MCInst;
class StringRef;
class Target;
class SMLoc;
class AsmToken;
class MCParsedAsmOperand;
template <typename T> class SmallVectorImpl;

/// TargetAsmParser - Generic interface to target specific assembly parsers.
class TargetAsmParser {
  TargetAsmParser(const TargetAsmParser &);   // DO NOT IMPLEMENT
  void operator=(const TargetAsmParser &);  // DO NOT IMPLEMENT
protected: // Can only create subclasses.
  TargetAsmParser(const Target &);
 
  /// TheTarget - The Target that this machine was created for.
  const Target &TheTarget;

public:
  virtual ~TargetAsmParser();

  const Target &getTarget() const { return TheTarget; }

  /// ParseInstruction - Parse one assembly instruction.
  ///
  /// The parser is positioned following the instruction name. The target
  /// specific instruction parser should parse the entire instruction and
  /// construct the appropriate MCInst, or emit an error. On success, the entire
  /// line should be parsed up to and including the end-of-statement token. On
  /// failure, the parser is not required to read to the end of the line.
  //
  /// \param Name - The instruction name.
  /// \param NameLoc - The source location of the name.
  /// \param Operands [out] - The list of parsed operands, this returns
  ///        ownership of them to the caller.
  /// \return True on failure.
  virtual bool ParseInstruction(const StringRef &Name, SMLoc NameLoc,
                            SmallVectorImpl<MCParsedAsmOperand*> &Operands) = 0;

  /// ParseDirective - Parse a target specific assembler directive
  ///
  /// The parser is positioned following the directive name.  The target
  /// specific directive parser should parse the entire directive doing or
  /// recording any target specific work, or return true and do nothing if the
  /// directive is not target specific. If the directive is specific for
  /// the target, the entire line is parsed up to and including the
  /// end-of-statement token and false is returned.
  ///
  /// \param DirectiveID - the identifier token of the directive.
  virtual bool ParseDirective(AsmToken DirectiveID) = 0;
  
  /// MatchInstruction - Recognize a series of operands of a parsed instruction
  /// as an actual MCInst.  This returns false and fills in Inst on success and
  /// returns true on failure to match.
  virtual bool 
  MatchInstruction(const SmallVectorImpl<MCParsedAsmOperand*> &Operands,
                   MCInst &Inst) = 0;
  
};

} // End llvm namespace

#endif
