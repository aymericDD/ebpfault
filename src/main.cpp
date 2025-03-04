/*
  Copyright (c) 2019-present, Trail of Bits, Inc.
  All rights reserved.

  This source code is licensed in accordance with the terms specified in
  the LICENSE file found in the root directory of this source tree.
*/

#include "configuration.h"
#include "faultinjector.h"
#include "utils.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>

#include <tob/ebpf/perfeventarray.h>
#include <tob/utils/bufferreader.h>

#define EBPFAULT_DUMP_REGISTER(register_name)                                  \
  do {                                                                         \
    std::cout << std::setfill(' ') << std::setw(10) << #register_name << " "   \
              << std::hex << std::setfill('0') << std::setw(16)                \
              << event_data.register_name << " ";                              \
  } while (false)

std::unordered_map<std::uint64_t, std::string> event_name_map;

std::optional<tob::ebpfault::FaultInjector::EventData>
getEventData(tob::utils::BufferReader &buffer_reader) {
  tob::ebpfault::FaultInjector::EventData event_data;

  try {
    event_data.timestamp = buffer_reader.u64();
    event_data.event_id = buffer_reader.u64();
    event_data.process_id = buffer_reader.u32();
    event_data.thread_id = buffer_reader.u32();
    event_data.injected_error = buffer_reader.u64();
    event_data.r15 = buffer_reader.u64();
    event_data.r14 = buffer_reader.u64();
    event_data.r13 = buffer_reader.u64();
    event_data.r12 = buffer_reader.u64();
    event_data.rbp = buffer_reader.u64();
    event_data.rbx = buffer_reader.u64();
    event_data.r11 = buffer_reader.u64();
    event_data.r10 = buffer_reader.u64();
    event_data.r9 = buffer_reader.u64();
    event_data.r8 = buffer_reader.u64();
    event_data.rax = buffer_reader.u64();
    event_data.rcx = buffer_reader.u64();
    event_data.rdx = buffer_reader.u64();
    event_data.rsi = buffer_reader.u64();
    event_data.rdi = buffer_reader.u64();
    event_data.orig_rax = buffer_reader.u64();
    event_data.rip = buffer_reader.u64();
    event_data.cs = buffer_reader.u64();
    event_data.eflags = buffer_reader.u64();
    event_data.rsp = buffer_reader.u64();
    event_data.ss = buffer_reader.u64();

    return event_data;

  } catch (...) {
    return std::nullopt;
  }
}

void printEventData(tob::utils::BufferReader &buffer_reader,
                    tob::ebpf::PerfEventArray::BufferList buffer_list) {

  for (const auto &buffer : buffer_list) {
    buffer_reader.reset(buffer);
    buffer_reader.skipBytes(tob::ebpf::kPerfEventHeaderSize +
                            sizeof(std::uint32_t));

    auto opt_event_data = getEventData(buffer_reader);
    if (!opt_event_data.has_value()) {
      std::cerr << "Read error. Skipping event\n";
      continue;
    }

    const auto &event_data = opt_event_data.value();

    std::string event_name;
    auto event_name_it = event_name_map.find(event_data.event_id);
    if (event_name_it == event_name_map.end()) {
      event_name = std::to_string(event_data.event_id);
    } else {
      event_name = event_name_it->second;
    }

    std::cout << "timestamp: " << std::dec << event_data.timestamp
              << " syscall: " << event_name
              << " process_id: " << event_data.process_id
              << " thread_id: " << event_data.thread_id << " injected_error: "
              << tob::ebpfault::describeFaultValue(event_data.injected_error)
              << "\n";

    EBPFAULT_DUMP_REGISTER(r15);
    EBPFAULT_DUMP_REGISTER(r14);
    EBPFAULT_DUMP_REGISTER(r13);
    std::cout << "\n";

    EBPFAULT_DUMP_REGISTER(r12);
    EBPFAULT_DUMP_REGISTER(rbp);
    EBPFAULT_DUMP_REGISTER(rbx);
    std::cout << "\n";

    EBPFAULT_DUMP_REGISTER(r11);
    EBPFAULT_DUMP_REGISTER(r10);
    EBPFAULT_DUMP_REGISTER(r9);
    std::cout << "\n";

    EBPFAULT_DUMP_REGISTER(r8);
    EBPFAULT_DUMP_REGISTER(rax);
    EBPFAULT_DUMP_REGISTER(rcx);
    std::cout << "\n";

    EBPFAULT_DUMP_REGISTER(rdx);
    EBPFAULT_DUMP_REGISTER(rsi);
    EBPFAULT_DUMP_REGISTER(rdi);
    std::cout << "\n";

    EBPFAULT_DUMP_REGISTER(orig_rax);
    EBPFAULT_DUMP_REGISTER(rip);
    EBPFAULT_DUMP_REGISTER(cs);
    std::cout << "\n";

    EBPFAULT_DUMP_REGISTER(eflags);
    EBPFAULT_DUMP_REGISTER(rsp);
    EBPFAULT_DUMP_REGISTER(ss);
    std::cout << "\n\n";
  }
}

int main(int argc, char *argv[], char *envp[]) {
  auto command_line_params_exp = tob::ebpfault::parseCommandLine(argc, argv);
  if (!command_line_params_exp.succeeded()) {
    std::cerr << command_line_params_exp.error().message() << "\n";
    return 1;
  }

  auto command_line_params = command_line_params_exp.takeValue();
  static_cast<void>(command_line_params);

  if (!tob::ebpfault::configureRlimit()) {
    std::cerr << "Failed to set RLIMIT_MEMLOCK\n";
    return 1;
  }

  auto configuration_exp = tob::ebpfault::Configuration::create(
      command_line_params.configuration_path);

  if (!configuration_exp.succeeded()) {
    std::cerr << configuration_exp.error().message() << "\n";
    return 1;
  }

  auto configuration = configuration_exp.takeValue();

  auto perf_event_array_exp = tob::ebpf::PerfEventArray::create(10);

  if (!perf_event_array_exp.succeeded()) {
    std::cerr << perf_event_array_exp.error().message() << "\n";
    return 1;
  }

  auto perf_event_array = perf_event_array_exp.takeValue();

  tob::ebpfault::FaultInjector::ProcessIDFilter pid_filter;
  sem_t *execve_semaphore{nullptr};

  if (command_line_params.opt_pid_list.has_value()) {
    pid_filter.process_id_list = command_line_params.opt_pid_list.value();

    if (command_line_params.except_pid_list) {
      pid_filter.type =
          tob::ebpfault::FaultInjector::ProcessIDFilter::Type::Except;
    } else {
      pid_filter.type =
          tob::ebpfault::FaultInjector::ProcessIDFilter::Type::Matching;
    }

  } else {
    auto semaphore_name = "ebpfault_" + std::to_string(getpid());
    execve_semaphore =
        sem_open(semaphore_name.c_str(), O_CREAT | O_EXCL, 0600, 0);

    if (execve_semaphore == SEM_FAILED) {
      std::cerr << "Failed to create the semaphore\n";
      return 1;
    }

    auto child_pid = fork();

    if (child_pid == 0) {
      auto exec_command_line =
          command_line_params.opt_exec_command_line.value();

      auto path = exec_command_line.at(0);
      // exec_command_line.erase(exec_command_line.begin());

      std::vector<char *> exec_argv;
      for (auto &param : exec_command_line) {
        exec_argv.push_back(&param[0]);
      }

      exec_argv.push_back(nullptr);

      execve_semaphore = sem_open(semaphore_name.c_str(), O_CREAT, 0600, 0);

      if (execve_semaphore == SEM_FAILED) {
        std::cerr << "Failed to create the semaphore\n";
        return 1;
      }

      if (sem_wait(execve_semaphore) < 0) {
        std::cerr << "Semaphore wait has failed\n";
        return 1;
      }

      execve(path.c_str(), exec_argv.data(), envp);
      throw std::runtime_error("exec has failed");
    }

    pid_filter.process_id_list = {child_pid};
    pid_filter.type =
        tob::ebpfault::FaultInjector::ProcessIDFilter::Type::Matching;
  }

  std::vector<tob::ebpfault::FaultInjector::Ref> fault_injector_list;

  std::cout << "Generating fault injectors...\n\n";

  for (const auto &config : *configuration) {
    std::cout << " > " << config.name << "\n";
    std::cout << "   Error list:\n";

    for (const auto &error : config.error_list) {
      std::cout << "   - " << std::setw(3) << std::setfill(' ')
                << static_cast<int>(error.probability) << "% => "
                << tob::ebpfault::describeFaultValue(error.exit_code) << "\n";
    }

    auto fault_injector_exp = tob::ebpfault::FaultInjector::create(
        *perf_event_array.get(), config, pid_filter);

    if (!fault_injector_exp.succeeded()) {
      std::cerr << fault_injector_exp.error().message() << "\n";
      return 1;
    }

    auto fault_injector = fault_injector_exp.takeValue();

    event_name_map.insert({fault_injector->eventIdentifier(), config.name});

    fault_injector_list.push_back(std::move(fault_injector));

    std::cout << "\n";
  }

  if (execve_semaphore != nullptr) {
    if (sem_post(execve_semaphore) < 0) {
      std::cerr << "Failed to post to the semaphore\n";
      return 1;
    }
  }

  auto buffer_reader_exp = tob::utils::BufferReader::create();
  if (!buffer_reader_exp.succeeded()) {
    std::cerr << "Failed to create the buffer reader: "
              << buffer_reader_exp.error().message() << "\n";
    return 1;
  }

  auto buffer_reader = buffer_reader_exp.takeValue();

  for (;;) {
    std::size_t running_process_count = 0U;

    for (auto pid : pid_filter.process_id_list) {
      if (kill(pid, 0) == 0) {
        ++running_process_count;
      }
    }

    if (running_process_count == 0U) {
      std::cout << "All processes have been terminated\n";
      break;
    }

    tob::ebpf::PerfEventArray::BufferList buffer_list;
    std::size_t read_error_count{};
    std::size_t lost_event_count{};

    if (!perf_event_array->read(buffer_list, read_error_count,
                                lost_event_count)) {
      continue;
    }

    printEventData(*buffer_reader.get(), std::move(buffer_list));
  }

  if (execve_semaphore != nullptr) {
    sem_close(execve_semaphore);
  }

  std::cout << "Exiting...\n";
  return 0;
}
