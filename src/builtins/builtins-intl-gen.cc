// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTL_SUPPORT
#error Internationalization is expected to be enabled.
#endif  // V8_INTL_SUPPORT

#include "src/builtins/builtins-iterator-gen.h"
#include "src/builtins/builtins-utils-gen.h"
#include "src/codegen/code-stub-assembler.h"
#include "src/objects/js-list-format-inl.h"
#include "src/objects/js-list-format.h"
#include "src/objects/objects-inl.h"
#include "src/objects/objects.h"

namespace v8 {
namespace internal {

template <class T>
using TNode = compiler::TNode<T>;

class IntlBuiltinsAssembler : public CodeStubAssembler {
 public:
  explicit IntlBuiltinsAssembler(compiler::CodeAssemblerState* state)
      : CodeStubAssembler(state) {}

  void ListFormatCommon(TNode<Context> context, TNode<Int32T> argc,
                        Runtime::FunctionId format_func_id,
                        const char* method_name);

  TNode<JSArray> AllocateEmptyJSArray(TNode<Context> context);
};

TF_BUILTIN(StringToLowerCaseIntl, IntlBuiltinsAssembler) {
  TNode<String> const string = CAST(Parameter(Descriptor::kString));

  Label call_c(this), return_string(this), runtime(this, Label::kDeferred);

  // Early exit on empty strings.
  TNode<Uint32T> const length = LoadStringLengthAsWord32(string);
  GotoIf(Word32Equal(length, Uint32Constant(0)), &return_string);

  // Unpack strings if possible, and bail to runtime unless we get a one-byte
  // flat string.
  ToDirectStringAssembler to_direct(
      state(), string, ToDirectStringAssembler::kDontUnpackSlicedStrings);
  to_direct.TryToDirect(&runtime);

  TNode<Int32T> const instance_type = to_direct.instance_type();
  CSA_ASSERT(this,
             Word32BinaryNot(IsIndirectStringInstanceType(instance_type)));
  GotoIfNot(IsOneByteStringInstanceType(instance_type), &runtime);

  // For short strings, do the conversion in CSA through the lookup table.

  Node* const dst = AllocateSeqOneByteString(length);

  const int kMaxShortStringLength = 24;  // Determined empirically.
  GotoIf(Uint32GreaterThan(length, Uint32Constant(kMaxShortStringLength)),
         &call_c);

  {
    Node* const dst_ptr = PointerToSeqStringData(dst);
    VARIABLE(var_cursor, MachineType::PointerRepresentation(),
             IntPtrConstant(0));

    TNode<RawPtrT> const start_address = to_direct.PointerToData(&call_c);
    TNode<IntPtrT> const end_address =
        Signed(IntPtrAdd(start_address, ChangeUint32ToWord(length)));

    Node* const to_lower_table_addr =
        ExternalConstant(ExternalReference::intl_to_latin1_lower_table());

    VARIABLE(var_did_change, MachineRepresentation::kWord32, Int32Constant(0));

    VariableList push_vars({&var_cursor, &var_did_change}, zone());
    BuildFastLoop(push_vars, start_address, end_address,
                  [=, &var_cursor, &var_did_change](Node* current) {
                    Node* c = Load(MachineType::Uint8(), current);
                    Node* lower =
                        Load(MachineType::Uint8(), to_lower_table_addr,
                             ChangeInt32ToIntPtr(c));
                    StoreNoWriteBarrier(MachineRepresentation::kWord8, dst_ptr,
                                        var_cursor.value(), lower);

                    var_did_change.Bind(Word32Or(Word32NotEqual(c, lower),
                                                 var_did_change.value()));

                    Increment(&var_cursor);
                  },
                  kCharSize, INTPTR_PARAMETERS, IndexAdvanceMode::kPost);

    // Return the original string if it remained unchanged in order to preserve
    // e.g. internalization and private symbols (such as the preserved object
    // hash) on the source string.
    GotoIfNot(var_did_change.value(), &return_string);

    Return(dst);
  }

  // Call into C for case conversion. The signature is:
  // String ConvertOneByteToLower(String src, String dst);
  BIND(&call_c);
  {
    TNode<String> const src = to_direct.string();

    Node* const function_addr =
        ExternalConstant(ExternalReference::intl_convert_one_byte_to_lower());

    MachineType type_tagged = MachineType::AnyTagged();

    Node* const result = CallCFunction(function_addr, type_tagged,
                                       std::make_pair(type_tagged, src),
                                       std::make_pair(type_tagged, dst));

    Return(result);
  }

  BIND(&return_string);
  Return(string);

  BIND(&runtime);
  {
    Node* const result = CallRuntime(Runtime::kStringToLowerCaseIntl,
                                     NoContextConstant(), string);
    Return(result);
  }
}

TF_BUILTIN(StringPrototypeToLowerCaseIntl, IntlBuiltinsAssembler) {
  TNode<Object> maybe_string = CAST(Parameter(Descriptor::kReceiver));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  TNode<String> string =
      ToThisString(context, maybe_string, "String.prototype.toLowerCase");

  Return(CallBuiltin(Builtins::kStringToLowerCaseIntl, context, string));
}

void IntlBuiltinsAssembler::ListFormatCommon(TNode<Context> context,
                                             TNode<Int32T> argc,
                                             Runtime::FunctionId format_func_id,
                                             const char* method_name) {
  CodeStubArguments args(this, ChangeInt32ToIntPtr(argc));

  // Label has_list(this);
  // 1. Let lf be this value.
  // 2. If Type(lf) is not Object, throw a TypeError exception.
  TNode<Object> receiver = args.GetReceiver();

  // 3. If lf does not have an [[InitializedListFormat]] internal slot, throw a
  // TypeError exception.
  ThrowIfNotInstanceType(context, receiver, JS_INTL_LIST_FORMAT_TYPE,
                         method_name);
  TNode<JSListFormat> list_format = CAST(receiver);

  // 4. If list is not provided or is undefined, then
  TNode<Object> list = args.GetOptionalArgumentValue(0);
  Label has_list(this);
  {
    GotoIfNot(IsUndefined(list), &has_list);
    if (format_func_id == Runtime::kFormatList) {
      // a. Return an empty String.
      args.PopAndReturn(EmptyStringConstant());
    } else {
      DCHECK_EQ(format_func_id, Runtime::kFormatListToParts);
      // a. Return an empty Array.
      args.PopAndReturn(AllocateEmptyJSArray(context));
    }
  }
  BIND(&has_list);
  {
    // 5. Let x be ? IterableToList(list).
    TNode<Object> x =
        CallBuiltin(Builtins::kIterableToListWithSymbolLookup, context, list);

    // 6. Return ? FormatList(lf, x).
    args.PopAndReturn(CallRuntime(format_func_id, context, list_format, x));
  }
}

TNode<JSArray> IntlBuiltinsAssembler::AllocateEmptyJSArray(
    TNode<Context> context) {
  return CodeStubAssembler::AllocateJSArray(
      PACKED_ELEMENTS,
      LoadJSArrayElementsMap(PACKED_ELEMENTS, LoadNativeContext(context)),
      SmiConstant(0), SmiConstant(0));
}

TF_BUILTIN(ListFormatPrototypeFormat, IntlBuiltinsAssembler) {
  ListFormatCommon(
      CAST(Parameter(Descriptor::kContext)),
      UncheckedCast<Int32T>(Parameter(Descriptor::kJSActualArgumentsCount)),
      Runtime::kFormatList, "Intl.ListFormat.prototype.format");
}

TF_BUILTIN(ListFormatPrototypeFormatToParts, IntlBuiltinsAssembler) {
  ListFormatCommon(
      CAST(Parameter(Descriptor::kContext)),
      UncheckedCast<Int32T>(Parameter(Descriptor::kJSActualArgumentsCount)),
      Runtime::kFormatListToParts, "Intl.ListFormat.prototype.formatToParts");
}

}  // namespace internal
}  // namespace v8
