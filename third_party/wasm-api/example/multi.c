#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "wasm.h"

#define own

// A function to be called from Wasm code.
own wasm_trap_t* callback(
  const wasm_val_t args[], wasm_val_t results[]
) {
  printf("Calling back...\n> ");
  printf("> %"PRIu32" %"PRIu64" %"PRIu64" %"PRIu32"\n",
    args[0].of.i32, args[1].of.i64, args[2].of.i64, args[3].of.i32);
  printf("\n");

  wasm_val_copy(&results[0], &args[0]);
  return NULL;
}


// A function closure.
own wasm_trap_t* closure_callback(
  void* env, const wasm_val_t args[], wasm_val_t results[]
) {
  int i = *(int*)env;
  printf("Calling back closure...\n");
  printf("> %d\n", i);

  results[0].kind = WASM_I32;
  results[0].of.i32 = (int32_t)i;
  return NULL;
}


int main(int argc, const char* argv[]) {
  // Initialize.
  printf("Initializing...\n");
  wasm_engine_t* engine = wasm_engine_new();
  wasm_store_t* store = wasm_store_new(engine);

  // Load binary.
  printf("Loading binary...\n");
  FILE* file = fopen("multi.wasm", "r");
  if (!file) {
    printf("> Error loading module!\n");
    return 1;
  }
  fseek(file, 0L, SEEK_END);
  size_t file_size = ftell(file);
  fseek(file, 0L, SEEK_SET);
  wasm_byte_vec_t binary;
  wasm_byte_vec_new_uninitialized(&binary, file_size);
  if (fread(binary.data, file_size, 1, file) != 1) {
    printf("> Error loading module!\n");
    return 1;
  }
  fclose(file);

  // Compile.
  printf("Compiling module...\n");
  own wasm_module_t* module = wasm_module_new(store, &binary);
  if (!module) {
    printf("> Error compiling module!\n");
    return 1;
  }

  wasm_byte_vec_delete(&binary);

  // Create external print functions.
  printf("Creating callback...\n");
  wasm_valtype_t* types[4] = {
    wasm_valtype_new_i32(), wasm_valtype_new_i64(),
    wasm_valtype_new_i64(), wasm_valtype_new_i32()
  };
  own wasm_valtype_vec_t tuple1, tuple2;
  wasm_valtype_vec_new(&tuple1, 4, types);
  wasm_valtype_vec_copy(&tuple2, &tuple1);
  own wasm_functype_t* callback_type = wasm_functype_new(&tuple1, &tuple2);
  own wasm_func_t* callback_func = wasm_func_new(store, callback_type, callback);

  wasm_functype_delete(callback_type);

  // Instantiate.
  printf("Instantiating module...\n");
  const wasm_extern_t* imports[] = {wasm_func_as_extern(callback_func)};
  own wasm_instance_t* instance = wasm_instance_new(store, module, imports);
  if (!instance) {
    printf("> Error instantiating module!\n");
    return 1;
  }

  wasm_func_delete(callback_func);

  // Extract export.
  printf("Extracting export...\n");
  own wasm_extern_vec_t exports;
  wasm_instance_exports(instance, &exports);
  if (exports.size == 0) {
    printf("> Error accessing exports!\n");
    return 1;
  }
  const wasm_func_t* run_func = wasm_extern_as_func(exports.data[0]);
  if (run_func == NULL) {
    printf("> Error accessing export!\n");
    return 1;
  }

  wasm_module_delete(module);
  wasm_instance_delete(instance);

  // Call.
  printf("Calling export...\n");
  wasm_val_t args[4];
  args[0].kind = WASM_I32;
  args[0].of.i32 = 1;
  args[1].kind = WASM_I64;
  args[1].of.i64 = 2;
  args[2].kind = WASM_I64;
  args[2].of.i64 = 3;
  args[3].kind = WASM_I32;
  args[3].of.i32 = 4;
  wasm_val_t results[4];
  if (wasm_func_call(run_func, args, results)) {
    printf("> Error calling function!\n");
    return 1;
  }

  wasm_extern_vec_delete(&exports);

  // Print result.
  printf("Printing result...\n");
  printf("> %"PRIu32" %"PRIu64" %"PRIu64" %"PRIu32"\n",
    results[0].of.i32, results[1].of.i64,
    results[2].of.i64, results[3].of.i32);

  assert(results[0].of.i32 == 4);
  assert(results[1].of.i64 == 3);
  assert(results[2].of.i64 == 2);
  assert(results[3].of.i32 == 1);

  // Shut down.
  printf("Shutting down...\n");
  wasm_store_delete(store);
  wasm_engine_delete(engine);

  // All done.
  printf("Done.\n");
  return 0;
}
