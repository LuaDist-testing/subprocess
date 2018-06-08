
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "compat-5.3.h"
#include "close_fds.h"
#include "../inheritable.h"

/* Returns 1 if there is a problem with fd_sequence, 0 otherwise. */
static int fd_sequence_is_ok(lua_State* L, int idx) {
   lua_Integer len;
   int i;
   int prev_fd = -1;
   
   len = luaL_len(L, idx);
   
   for (i = 0; i < len; i++) {
      lua_Integer value;
      int valtype = lua_geti(L, idx, i + 1);
      if (valtype != LUA_TNUMBER) {
         return 0;
      }
      value = lua_tointeger(L, -1);
      lua_pop(L, 1);
      if (value < 0 || value < prev_fd || value > INT_MAX) {
         return 0;
      }
      prev_fd = value;
   }
   return 1;
}

void free_c_string_array(char** arr) {
   char** at = arr;
   while (*at) {
      free(*at);
      at++;
   }
   free(arr);
}

char** array_of_strings_to_c(lua_State* L, int idx) {
   lua_Integer len;
   int i = 0;
   int j = 0;
   char** ret;
   char* walk;
   
   len = luaL_len(L, idx);
   ret = calloc(len + 1, sizeof(char*));
   if (!ret) {
      return NULL;
   }
   ret[len] = NULL;
   
   for (i = 0; i < len; i++) {
      int valtype = lua_geti(L, idx, i + 1);
      if (valtype != LUA_TSTRING) {
         goto failure;
      }
      ret[i] = strdup(lua_tostring(L, -1));
//fprintf(stderr, "[%d] %s\n", i, ret[i]);
      lua_pop(L, 1);
   }
   return ret;
   
failure:
   free_c_string_array(ret);
   return NULL;
}

int make_inheritable(lua_State* L, int FDS_TO_KEEP, lua_Integer errpipe_write) {
   lua_Integer len;
   int i = 0;
   len = luaL_len(L, FDS_TO_KEEP);
   for (i = 0; i < len; ++i) {
      long fd;
      lua_geti(L, FDS_TO_KEEP, i + 1);
      fd = lua_tonumber(L, -1);
      lua_pop(L, 1);
      assert(0 <= fd && fd <= INT_MAX);
      if (fd == errpipe_write) {
         set_inheritable((int)fd, 0);
         continue;
      }
      if (set_inheritable((int)fd, 1) < 0) {
         return -1;
      }
   }
   return 0;
}

#define POSIX_CALL(call)   do { if ((call) == -1) goto error; } while (0)

int child_exec(char** exec_array, char** argv, char** envp, const char* cwd,
               lua_Integer p2cread, lua_Integer p2cwrite,
               lua_Integer c2pread, lua_Integer c2pwrite,
               lua_Integer errread, lua_Integer errwrite,
               lua_Integer errpipe_read, lua_Integer errpipe_write,
               int close_fds, int start_new_session,
               lua_State* L, int FDS_TO_KEEP) {

   int i, saved_errno, unused, reached_preexec = 0, n = 0;

   const char* err_msg = "";
   /* Buffer large enough to hold a hex integer.  We can't malloc. */
   char hex_errno[sizeof(saved_errno)*2+1];

//fprintf(stderr, "errpipe_write %d\n", errpipe_write);

   if (make_inheritable(L, FDS_TO_KEEP, errpipe_write) < 0) {
      goto error;
   }

   /* Close parent's pipe ends. */
   if (p2cwrite != -1) {
      POSIX_CALL(close(p2cwrite));
   }
   if (c2pread != -1) {
      POSIX_CALL(close(c2pread));
   }
   if (errread != -1) {
      POSIX_CALL(close(errread));
   }
   POSIX_CALL(close(errpipe_read));

   /* When duping fds, if there arises a situation where one of the fds is
      either 0, 1 or 2, it is possible that it is overwritten (#12607). */
   if (c2pwrite == 0) {
      POSIX_CALL(c2pwrite = dup(c2pwrite));
   }
   if (errwrite == 0 || errwrite == 1) {
      POSIX_CALL(errwrite = dup(errwrite));
   }

   /* Dup fds for child.
      dup2() removes the CLOEXEC flag but we must do it ourselves if dup2()
      would be a no-op (issue #10806). */
   if (p2cread == 0) {
      if (set_inheritable(p2cread, 1) < 0) {
         goto error;
      }
   } else if (p2cread != -1) {
      POSIX_CALL(dup2(p2cread, 0));  /* stdin */
   }

   if (c2pwrite == 1) {
      if (set_inheritable(c2pwrite, 1) < 0) {
         goto error;
      }
   } else if (c2pwrite != -1) {
      POSIX_CALL(dup2(c2pwrite, 1));  /* stdout */
   }

   if (errwrite == 2) {
      if (set_inheritable(errwrite, 1) < 0) {
         goto error;
      }
   } else if (errwrite != -1) {
      POSIX_CALL(dup2(errwrite, 2));  /* stderr */
   }

   /* Close pipe fds.  Make sure we don't close the same fd more than */
   /* once, or standard fds. */
   if (p2cread > 2) {
      POSIX_CALL(close(p2cread));
   }
   if (c2pwrite > 2 && c2pwrite != p2cread) {
      POSIX_CALL(close(c2pwrite));
   }
   if (errwrite != c2pwrite && errwrite != p2cread && errwrite > 2) {
      POSIX_CALL(close(errwrite));
   }

   if (cwd) {
      POSIX_CALL(chdir(cwd));
   }

   if (start_new_session) {
      POSIX_CALL(setsid());
   }

   if (close_fds) {
      close_open_fds(3, L, FDS_TO_KEEP);
   }

//FILE* tty2 = fopen("/dev/pts/2", "w");
//for (n = 0; argv[n]; n++) {
//fprintf(tty2, "[%d] %s\n", n, argv[n]);
//}
   reached_preexec = 1;
   /* This loop matches the Lib/os.py _execvpe()'s PATH search when */
   /* given the executable_list generated by Lib/subprocess.py.     */
   saved_errno = 0;
   for (i = 0; exec_array[i] != NULL; ++i) {
      const char *executable = exec_array[i];

//fprintf(tty2, "trying %s %p %p!\n", executable, argv, envp);
      if (envp) {
         execve(executable, argv, envp);
      } else {
         execv(executable, argv);
      }
//fprintf(tty2, "failed!\n");
      if (errno != ENOENT && errno != ENOTDIR && saved_errno == 0) {
         saved_errno = errno;
      }
   }
   /* Report the first exec error, not the last. */
   if (saved_errno) {
      errno = saved_errno;
   }

error:
   saved_errno = errno;
   /* Report the posix error to our parent process. */
   /* We ignore all write() return values as the total size of our writes is
    * less than PIPEBUF and we cannot do anything about an error anyways. */
   if (saved_errno) {
      char *cur;
      unused = write(errpipe_write, "OSError:", 8);
      cur = hex_errno + sizeof(hex_errno);
      while (saved_errno != 0 && cur > hex_errno) {
         *--cur = "0123456789ABCDEF"[saved_errno % 16];
         saved_errno /= 16;
      }
      unused = write(errpipe_write, cur, hex_errno + sizeof(hex_errno) - cur);
      unused = write(errpipe_write, ":", 1);
      if (!reached_preexec) {
         /* Indicate to the parent that the error happened before exec(). */
         unused = write(errpipe_write, "noexec", 6);
      }
   /* We can't call strerror(saved_errno).  It is not async signal safe.
    * The parent process will look the error message up. */
   } else {
      unused = write(errpipe_write, "SubprocessError:0:", 18);
      unused = write(errpipe_write, err_msg, strlen(err_msg));
   }
   if (unused) return;  /* silly? yes! avoids gcc compiler warning. */

}

int fork_exec(lua_State* L) {
   enum arg {
      ARGS = 1, EXECUTABLE_LIST, CLOSE_FDS, FDS_TO_KEEP, CWD, ENV_LIST,
      P2CREAD, P2CWRITE, C2PREAD, C2PWRITE, ERRREAD, ERRWRITE,
      ERRPIPE_READ, ERRPIPE_WRITE, START_NEW_SESSION
   };
   const char* cwd = NULL;
   lua_Integer p2cread = -1, p2cwrite = -1, c2pread = -1, c2pwrite = -1;
   lua_Integer errread = -1, errwrite = -1, errpipe_read = -1, errpipe_write = -1;
   int close_fds = 0, start_new_session = 0;

   char** argv = NULL;
   char** exec_array = NULL;
   char** envp = NULL;
   pid_t pid = -1;
   int save_errno = 0;
   
   luaL_checktype(L, ARGS, LUA_TTABLE);
   luaL_checktype(L, EXECUTABLE_LIST, LUA_TTABLE);
   close_fds = lua_toboolean(L, CLOSE_FDS);
   luaL_checktype(L, FDS_TO_KEEP, LUA_TTABLE);
   cwd = lua_tostring(L, CWD);
   if (lua_type(L, ENV_LIST) != LUA_TTABLE && lua_type(L, ENV_LIST) != LUA_TNIL) {
      lua_pushstring(L, "env_list expects table or nil");
      lua_error(L);
   }
   p2cread = luaL_checkinteger(L, P2CREAD);
   p2cwrite = luaL_checkinteger(L, P2CWRITE);
   c2pread = luaL_checkinteger(L, C2PREAD);
   c2pwrite = luaL_checkinteger(L, C2PWRITE);
   errread = luaL_checkinteger(L, ERRREAD);
   errwrite = luaL_checkinteger(L, ERRWRITE);
   errpipe_read = luaL_checkinteger(L, ERRPIPE_READ);
   errpipe_write = luaL_checkinteger(L, ERRPIPE_WRITE);
   start_new_session = lua_toboolean(L, START_NEW_SESSION);

   /* precondition */
   if (close_fds && errpipe_write < 3) {
      lua_pushstring(L, "errpipe_write must be >= 3");
      lua_error(L);
   }
   if (!fd_sequence_is_ok(L, FDS_TO_KEEP)) {
      lua_pushstring(L, "bad value(s) in fds_to_keep");
      lua_error(L);
   }

   argv = array_of_strings_to_c(L, ARGS);
   if (!argv) {
      goto teardown;
   }
   
   exec_array = array_of_strings_to_c(L, EXECUTABLE_LIST);
   if (!exec_array) {
      goto teardown;
   }

   if (lua_type(L, ENV_LIST) == LUA_TTABLE) {
//fprintf(stderr, "Making envp");
      envp = array_of_strings_to_c(L, ENV_LIST);
      if (!envp) {
         goto teardown;
      }
   }

   pid = fork();
   if (pid == 0) {
      /* Child process */
      /*
       * Code from here to _exit() must only use async-signal-safe functions,
       * listed at `man 7 signal` or
       * http://www.opengroup.org/onlinepubs/009695399/functions/xsh_chap02_04.html.
       */
      child_exec(exec_array, argv, envp, cwd,
                 p2cread, p2cwrite, c2pread, c2pwrite,
                 errread, errwrite, errpipe_read, errpipe_write,
                 close_fds, start_new_session, L, FDS_TO_KEEP);
      _exit(255);
      return 0;  /* Dead code to avoid a potential compiler warning. */
   }

   if (pid == -1) {
      /* Capture the errno exception before errno can be clobbered. */
      save_errno = errno;
   }

teardown:

   /* Parent process */
   if (envp) {
      free_c_string_array(envp);
   }
   if (argv) {
      free_c_string_array(argv);
   }
   if (exec_array) {
      free_c_string_array(exec_array);
   }

   if (pid <= 0) {
      char message[255];
      snprintf(message, sizeof(message)-1, "Failed forking: (%d) %s", save_errno, strerror(save_errno));
      lua_pushstring(L, message);
      lua_error(L);
   }
   lua_pushnumber(L, pid);
   return 1;
}


static luaL_Reg functions[] = {
   { "fork_exec", fork_exec },
   NULL
};

int luaopen_subprocess_posix_core(lua_State* L) {
   luaL_newlib(L, functions);
   return 1;
}