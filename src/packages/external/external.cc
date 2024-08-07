#include "base/package_api.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>  // for exit
#include <thread>
#include <string>
#include <fmt/format.h>

#include <event2/event.h>

#include "include/socket_err.h"
#include "packages/sockets/socket_efuns.h"

#ifndef _WIN32
#include <sstream>
#include <vector>
#include <spawn.h>
#include <sys/wait.h>

template <typename Out>
void split(const std::string &s, char delim, Out result) {
  std::istringstream iss(s);
  std::string item;
  while (std::getline(iss, item, delim)) {
    *result++ = item;
  }
}

// Added because debug() macro won't take a struct tm as an argument.
std::string format_time(const struct tm& timeinfo) {
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return std::string(buffer);
}

int external_start(int which, svalue_t *args, svalue_t *arg1, svalue_t *arg2, svalue_t *arg3) {
  std::vector<std::string> newargs_data = {std::string(external_cmd[which])};
  if (args->type == T_ARRAY) {
    for (int i = 0; i < args->u.arr->size; i++) {
      auto item = args->u.arr->item[i];
      if (item.type != T_STRING) {
        error("Bad argument list item %d to external_start()\n", i);
      }
      newargs_data.push_back(item.u.string);
    }
  } else {
    split(std::string(args->u.string), ' ', std::back_inserter(newargs_data));
  }

  std::vector<char *> newargs;
  for (auto &arg : newargs_data) {
    newargs.push_back(arg.data());
  }
  newargs.push_back(nullptr);

  posix_spawn_file_actions_t file_actions;
  int ret = posix_spawn_file_actions_init(&file_actions);
  if (ret != 0) {
    debug(external_start, "external_start: posix_spawn_file_actions_init() error: %s\n", strerror(ret));
    return EESOCKET;
  }
  DEFER { posix_spawn_file_actions_destroy(&file_actions); };

  evutil_socket_t sv[2];
  if (evutil_socketpair(PF_UNIX, SOCK_STREAM, 0, sv) == -1) {
    return EESOCKET;
  }
  DEFER {
    if (sv[0] > 0) {
      evutil_closesocket(sv[0]);
    }
    if (sv[1] > 0) {
      evutil_closesocket(sv[1]);
    }
  };
  if (evutil_make_socket_nonblocking(sv[0]) == -1 || evutil_make_socket_nonblocking(sv[1]) == -1) {
    return EESOCKET;
  }
  ret = posix_spawn_file_actions_adddup2(&file_actions, sv[1], 0) ||
        posix_spawn_file_actions_adddup2(&file_actions, sv[1], 1) ||
        posix_spawn_file_actions_adddup2(&file_actions, sv[1], 2);
  if (ret != 0) {
    debug(external_start, "external_start: posix_spawn_file_actions_adddup2() error: %s\n", strerror(ret));
    return EESOCKET;
  }

  int fd = find_new_socket();
  if (fd < 0) {
    return fd;
  }

  auto *sock = lpc_socks_get(fd);
  new_lpc_socket_event_listener(fd, sock, sv[0]);

  sock->fd = sv[0];
  sock->flags = S_EXTERNAL;
  set_read_callback(fd, arg1);
  set_write_callback(fd, arg2);
  set_close_callback(fd, arg3);
  sock->owner_ob = current_object;
  sock->mode = STREAM;
  sock->state = STATE_DATA_XFER;
  memset(reinterpret_cast<char *>(&sock->l_addr), 0, sizeof(sock->l_addr));
  memset(reinterpret_cast<char *>(&sock->r_addr), 0, sizeof(sock->r_addr));
  sock->owner_ob = current_object;
  sock->release_ob = nullptr;
  sock->r_buf = nullptr;
  sock->r_off = 0;
  sock->r_len = 0;
  sock->w_buf = nullptr;
  sock->w_off = 0;
  sock->w_len = 0;

  current_object->flags |= O_EFUN_SOCKET;

  event_add(sock->ev_write, nullptr);
  event_add(sock->ev_read, nullptr);

  pid_t pid;
  char *newenviron[] = {nullptr};
  ret = posix_spawn(&pid, newargs[0], &file_actions, nullptr, newargs.data(), newenviron);
  if (ret) {
    debug(external_start, "external_start: posix_spawn() error: %s\n", strerror(ret));
    return EESOCKET;
  }

  evutil_closesocket(sv[1]);
  sv[1] = -1;

  evutil_socket_t childfd = sv[0];
  sv[0] = -1;

  debug(external_start, "external_start: Launching external command '%s %s', pid: %jd.\n", external_cmd[which],
                args->type == T_STRING ? args->u.string : "<ARRAY>", (intmax_t)pid);

  std::thread([=]() {
    int status;
    do {
      const int s = waitpid(pid, &status, WUNTRACED | WCONTINUED);
      if (s == -1) {
        debug(external_start, "external_start: waitpid() error: %s (%d).\n", strerror(errno), errno);
        return;
      }
      std::string res = fmt::format(FMT_STRING("external_start(): child {} status: "), pid);
      if (WIFEXITED(status)) {
        res += fmt::format(FMT_STRING("exited, status={}\n"), WEXITSTATUS(status));
      } else if (WIFSIGNALED(status)) {
        res += fmt::format(FMT_STRING("killed by signal {}\n"), WTERMSIG(status));
      } else if (WIFSTOPPED(status)) {
        res += fmt::format(FMT_STRING("stopped by signal {}\n"), WSTOPSIG(status));
      } else if (WIFCONTINUED(status)) {
        res += "continued\n";
      }

      debug(external_start, "external_start: %s\n", format_time(res).c_str());
    } while (!WIFEXITED(status) && !WIFSIGNALED(status));
  }).detach();

  return fd;
}
#endif

namespace {
std::string quote_argument(const std::string &arg) {
  if (arg.empty()) {
    return "\"\"";
  }
  if (arg.find_first_of(" \t\n\v\"") == std::string::npos) {
    return arg;
  }
  std::string res = "\"";
  // from
  // https://learn.microsoft.com/en-us/archive/blogs/twistylittlepassagesallalike/everyone-quotes-command-line-arguments-the-wrong-way
  for (auto It = arg.begin();; ++It) {
    unsigned NumberBackslashes = 0;

    while (It != arg.end() && *It == '\\') {
      ++It;
      ++NumberBackslashes;
    }

    if (It == arg.end()) {
      //
      // Escape all backslashes, but let the terminating
      // double quotation mark we add below be interpreted
      // as a metacharacter.
      //
      res.append(NumberBackslashes * 2, '\\');
      break;
    } else if (*It == '"') {
      //
      // Escape all backslashes and the following
      // double quotation mark.
      //
      res.append(NumberBackslashes * 2 + 1, '\\');
      res.push_back(*It);
    } else {
      //
      // Backslashes aren't special here.
      //
      res.append(NumberBackslashes, '\\');
      res.push_back(*It);
    }
  }
  res.push_back('"');
  return res;
}
}  // namespace

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
extern int socketpair_win32(SOCKET socks[2], int make_overlapped);  // in socketpair.cc

int external_start(int which, svalue_t *args, svalue_t *arg1, svalue_t *arg2, svalue_t *arg3) {
  int fd;
  pid_t ret;

  std::string cmd = external_cmd[which];
  // guard against long path with spaces.
  cmd = trim(cmd, " ");
  if (cmd[0] != '"') {
    cmd = fmt::format("\"{}\"", cmd);
  }
  std::string cmdline = cmd + " ";

  if (args->type == T_ARRAY) {
    std::vector<std::string> argv(args->u.arr->size);
    for (int i = 0; i < args->u.arr->size; i++) {
      auto item = args->u.arr->item[i];
      if (item.type != T_STRING) {
        error("Bad argument list item %d to external_start()\n", i);
      }
      argv.emplace_back(quote_argument(item.u.string));
    }
    cmdline += fmt::to_string(fmt::join(argv.begin(), argv.end(), " "));
  } else {
    cmdline += std::string(args->u.string);
  }

  fd = find_new_socket();
  if (fd < 0) {
    return fd;
  }

  auto *sock = lpc_socks_get(fd);

  SOCKET sv[2];
  socketpair_win32(sv, 0);

  new_lpc_socket_event_listener(fd, sock, sv[1]);

  sock->fd = sv[1];
  sock->flags = S_EXTERNAL;
  set_read_callback(fd, arg1);
  set_write_callback(fd, arg2);
  set_close_callback(fd, arg3);
  sock->owner_ob = current_object;
  sock->mode = STREAM;
  sock->state = STATE_DATA_XFER;
  memset(reinterpret_cast<char *>(&sock->l_addr), 0, sizeof(sock->l_addr));
  memset(reinterpret_cast<char *>(&sock->r_addr), 0, sizeof(sock->r_addr));
  sock->owner_ob = current_object;
  sock->release_ob = NULL;
  sock->r_buf = NULL;
  sock->r_off = 0;
  sock->r_len = 0;
  sock->w_buf = NULL;
  sock->w_off = 0;
  sock->w_len = 0;

  current_object->flags |= O_EFUN_SOCKET;

  event_add(sock->ev_write, NULL);
  event_add(sock->ev_read, NULL);

  STARTUPINFOA si = {sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = reinterpret_cast<HANDLE>(sv[0]);
  si.hStdError = reinterpret_cast<HANDLE>(sv[0]);
  si.hStdOutput = reinterpret_cast<HANDLE>(sv[0]);
  PROCESS_INFORMATION processInfo{};

  // Start the child process.
  if (!CreateProcessA(NULL,            // No module name (use command line)
                      cmdline.data(),  // Command line
                      NULL,            // Process handle not inheritable
                      NULL,            // Thread handle not inheritable
                      TRUE,            // Set handle inheritance to TRUE
                      0,               // No creation flags
                      NULL,            // Use parent's environment block
                      NULL,            // Use parent's starting directory
                      &si,             // Pointer to STARTUPINFO structure
                      &processInfo)    // Pointer to PROCESS_INFORMATION structure
  ) {
    error("CreateProcess() in external_start() failed: %s\n", strerror(errno));
    return EESOCKET;
  }
  debug(external_start, "external_start: Launching external command '%s', pid: %d.\n", cmdline.c_str(),
                processInfo.dwProcessId);

  std::thread([=]() {
    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = -1;
    // Get the exit code.
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    debug(external_start, "external_start: pid: %d exited with %d.\n", processInfo.dwProcessId, exitCode);
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);
    evutil_closesocket(sv[0]);
  }).detach();

  return fd;
}
#endif

#ifdef F_EXTERNAL_START
void f_external_start() {
  int fd, num_arg = st_num_arg;
  svalue_t *arg = sp - num_arg + 1;

  if (!check_valid_socket("external", -1, current_object, "N/A", -1)) {
    pop_n_elems(num_arg - 1);
    sp->u.number = EESECURITY;
    return;
  }

  auto which = arg[0].u.number;
  if (--which < 0 || which > (g_num_external_cmds - 1) || !external_cmd[which]) {
    error("Bad argument 1 to external_start()\n");
  }

  fd = external_start(which, arg + 1, arg + 2, arg + 3, (num_arg == 5 ? arg + 4 : nullptr));
  pop_n_elems(num_arg - 1);
  sp->u.number = fd;
}
#endif
