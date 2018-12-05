/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jit_compiler.h"

#include "android-base/stringprintf.h"

#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "art_method-inl.h"
#include "base/logging.h"  // For VLOG
#include "base/stringpiece.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "debug/elf_debug_writer.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "jit/debugger_interface.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jit/jit_logger.h"

namespace art {
namespace jit {

JitCompiler* JitCompiler::Create() {
  return new JitCompiler();
}

void JitCompiler::ParseCompilerOptions() {
  // Special case max code units for inlining, whose default is "unset" (implictly
  // meaning no limit). Do this before parsing the actual passed options.
  compiler_options_->SetInlineMaxCodeUnits(CompilerOptions::kDefaultInlineMaxCodeUnits);
  Runtime* runtime = Runtime::Current();
  {
    std::string error_msg;
    if (!compiler_options_->ParseCompilerOptions(runtime->GetCompilerOptions(),
                                                /*ignore_unrecognized=*/ true,
                                                &error_msg)) {
      LOG(FATAL) << error_msg;
      UNREACHABLE();
    }
  }
  // JIT is never PIC, no matter what the runtime compiler options specify.
  compiler_options_->SetNonPic();

  // If the options don't provide whether we generate debuggable code, set
  // debuggability based on the runtime value.
  if (!compiler_options_->GetDebuggable()) {
    compiler_options_->SetDebuggable(runtime->IsJavaDebuggable());
  }

  const InstructionSet instruction_set = compiler_options_->GetInstructionSet();
  if (kRuntimeISA == InstructionSet::kArm) {
    DCHECK_EQ(instruction_set, InstructionSet::kThumb2);
  } else {
    DCHECK_EQ(instruction_set, kRuntimeISA);
  }
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features;
  for (const StringPiece option : runtime->GetCompilerOptions()) {
    VLOG(compiler) << "JIT compiler option " << option;
    std::string error_msg;
    if (option.starts_with("--instruction-set-variant=")) {
      StringPiece str = option.substr(strlen("--instruction-set-variant=")).data();
      VLOG(compiler) << "JIT instruction set variant " << str;
      instruction_set_features = InstructionSetFeatures::FromVariant(
          instruction_set, str.as_string(), &error_msg);
      if (instruction_set_features == nullptr) {
        LOG(WARNING) << "Error parsing " << option << " message=" << error_msg;
      }
    } else if (option.starts_with("--instruction-set-features=")) {
      StringPiece str = option.substr(strlen("--instruction-set-features=")).data();
      VLOG(compiler) << "JIT instruction set features " << str;
      if (instruction_set_features == nullptr) {
        instruction_set_features = InstructionSetFeatures::FromVariant(
            instruction_set, "default", &error_msg);
        if (instruction_set_features == nullptr) {
          LOG(WARNING) << "Error parsing " << option << " message=" << error_msg;
        }
      }
      instruction_set_features =
          instruction_set_features->AddFeaturesFromString(str.as_string(), &error_msg);
      if (instruction_set_features == nullptr) {
        LOG(WARNING) << "Error parsing " << option << " message=" << error_msg;
      }
    }
  }
  if (instruction_set_features == nullptr) {
    instruction_set_features = InstructionSetFeatures::FromCppDefines();
  }
  compiler_options_->instruction_set_features_ = std::move(instruction_set_features);
  compiler_options_->compiling_with_core_image_ =
      CompilerDriver::IsCoreImageFilename(runtime->GetImageLocation());

  if (compiler_options_->GetGenerateDebugInfo()) {
    jit_logger_.reset(new JitLogger());
    jit_logger_->OpenLog();
  }
}

extern "C" void* jit_load() {
  VLOG(jit) << "Create jit compiler";
  auto* const jit_compiler = JitCompiler::Create();
  CHECK(jit_compiler != nullptr);
  VLOG(jit) << "Done creating jit compiler";
  return jit_compiler;
}

extern "C" void jit_unload(void* handle) {
  DCHECK(handle != nullptr);
  delete reinterpret_cast<JitCompiler*>(handle);
}

extern "C" bool jit_compile_method(
    void* handle, ArtMethod* method, Thread* self, bool osr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  auto* jit_compiler = reinterpret_cast<JitCompiler*>(handle);
  DCHECK(jit_compiler != nullptr);
  return jit_compiler->CompileMethod(self, method, osr);
}

extern "C" void jit_types_loaded(void* handle, mirror::Class** types, size_t count)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  auto* jit_compiler = reinterpret_cast<JitCompiler*>(handle);
  DCHECK(jit_compiler != nullptr);
  const CompilerOptions& compiler_options = jit_compiler->GetCompilerOptions();
  if (compiler_options.GetGenerateDebugInfo()) {
    const ArrayRef<mirror::Class*> types_array(types, count);
    std::vector<uint8_t> elf_file = debug::WriteDebugElfFileForClasses(
        kRuntimeISA, compiler_options.GetInstructionSetFeatures(), types_array);
    // We never free debug info for types, so we don't need to provide a handle
    // (which would have been otherwise used as identifier to remove it later).
    AddNativeDebugInfoForJit(Thread::Current(),
                             /*code_ptr=*/ nullptr,
                             elf_file,
                             debug::PackElfFileForJIT,
                             compiler_options.GetInstructionSet(),
                             compiler_options.GetInstructionSetFeatures());
  }
}

extern "C" void jit_update_options(void* handle) {
  JitCompiler* jit_compiler = reinterpret_cast<JitCompiler*>(handle);
  DCHECK(jit_compiler != nullptr);
  jit_compiler->ParseCompilerOptions();
}

extern "C" bool jit_generate_debug_info(void* handle) {
  JitCompiler* jit_compiler = reinterpret_cast<JitCompiler*>(handle);
  DCHECK(jit_compiler != nullptr);
  return jit_compiler->GetCompilerOptions().GetGenerateDebugInfo();
}

JitCompiler::JitCompiler() {
  compiler_options_.reset(new CompilerOptions());
  ParseCompilerOptions();

  compiler_driver_.reset(new CompilerDriver(
      compiler_options_.get(),
      Compiler::kOptimizing,
      /* thread_count */ 1,
      /* swap_fd */ -1));
  // Disable dedupe so we can remove compiled methods.
  compiler_driver_->SetDedupeEnabled(false);
}

JitCompiler::~JitCompiler() {
  if (compiler_options_->GetGenerateDebugInfo()) {
    jit_logger_->CloseLog();
  }
}

bool JitCompiler::CompileMethod(Thread* self, ArtMethod* method, bool osr) {
  SCOPED_TRACE << "JIT compiling " << method->PrettyMethod();

  DCHECK(!method->IsProxyMethod());
  DCHECK(method->GetDeclaringClass()->IsResolved());

  TimingLogger logger(
      "JIT compiler timing logger", true, VLOG_IS_ON(jit), TimingLogger::TimingKind::kThreadCpu);
  self->AssertNoPendingException();
  Runtime* runtime = Runtime::Current();

  // Do the compilation.
  bool success = false;
  {
    TimingLogger::ScopedTiming t2("Compiling", &logger);
    JitCodeCache* const code_cache = runtime->GetJit()->GetCodeCache();
    success = compiler_driver_->GetCompiler()->JitCompile(
        self, code_cache, method, /* baseline= */ false, osr, jit_logger_.get());
  }

  // Trim maps to reduce memory usage.
  // TODO: move this to an idle phase.
  {
    TimingLogger::ScopedTiming t2("TrimMaps", &logger);
    runtime->GetJitArenaPool()->TrimMaps();
  }

  runtime->GetJit()->AddTimingLogger(logger);
  return success;
}

}  // namespace jit
}  // namespace art
