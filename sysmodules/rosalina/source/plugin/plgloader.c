#include <3ds.h>
#include "ifile.h"
#include "utils.h" // for makeARMBranch
#include "plugin.h"
#include "fmt.h"
#include "menu.h"
#include "menus.h"
#include "memory.h"
#include "sleep.h"
#include "task_runner.h"
#include "draw.h"

#define PLGLDR_VERSION (SYSTEM_VERSION(1, 0, 0))

#define THREADVARS_MAGIC  0x21545624 // !TV$

static const char *g_title = "Plugin loader";
PluginLoaderContext PluginLoaderCtx;
extern u32 g_blockMenuOpen;
extern u64 g_titleId;

void        IR__Patch(void);
void        IR__Unpatch(void);

bool PluginChecker_isEnabled = false;
bool RemoveDetector_isEnabled = false;
bool RemoveDetector_isRunning = false;

void        PluginLoader__Init(void)
{
    PluginLoaderContext *ctx = &PluginLoaderCtx;

    memset(ctx, 0, sizeof(PluginLoaderContext));

    s64 pluginLoaderFlags = 0;

    svcGetSystemInfo(&pluginLoaderFlags, 0x10000, 0x180);
    ctx->isEnabled =           pluginLoaderFlags & (1 << 0);
    PluginChecker_isEnabled  = pluginLoaderFlags & (1 << 1);
    RemoveDetector_isEnabled = pluginLoaderFlags & (1 << 2);

    ctx->plgEventPA = (s32 *)PA_FROM_VA_PTR(&ctx->plgEvent);
    ctx->plgReplyPA = (s32 *)PA_FROM_VA_PTR(&ctx->plgReply);

    MemoryBlock__ResetSwapSettings();

    assertSuccess(svcCreateAddressArbiter(&ctx->arbiter));
    assertSuccess(svcCreateEvent(&ctx->kernelEvent, RESET_ONESHOT));

    svcKernelSetState(0x10007, ctx->kernelEvent, 0, 0);
}

void    PluginLoader__Error(const char *message, Result res)
{
    DispErrMessage(g_title, message, res);
}

bool        PluginLoader__IsEnabled(void)
{
    return PluginLoaderCtx.isEnabled;
}

void        PluginLoader__MenuCallback(void)
{
    PluginLoaderCtx.isEnabled = !PluginLoaderCtx.isEnabled;
    RequestSaveSettings();
    PluginLoader__UpdateMenu();
}

void        PluginLoader__UpdateMenu(void)
{
    static const char *status[2] =
    {
        "Plugin Loader: [Disabled]",
        "Plugin Loader: [Enabled]"
    };

    rosalinaMenu.items[3].title = status[PluginLoaderCtx.isEnabled];
}

void        PluginChecker_MenuCallback(void)
{
    PluginChecker_isEnabled = !PluginChecker_isEnabled;
    SaveSettings();
    PluginChecker_UpdateMenu();
}

void        PluginChecker_UpdateMenu(void)
{
    static const char *status[2] =
    {
        "Plugin Checker: [Disabled]",
        "Plugin Checker: [Enabled]"
    };

    rosalinaMenu.items[4].title = status[PluginChecker_isEnabled];
}

void        RemoveDetector_MenuCallback(void)
{
    RemoveDetector_isEnabled = !RemoveDetector_isEnabled;
    SaveSettings();
    RemoveDetector_UpdateMenu();
}

void        RemoveDetector_UpdateMenu(void)
{
    static const char *status[2] =
    {
        "Remove Detector: [Disabled]",
        "Remove Detector: [Enabled]"
    };
    rosalinaMenu.items[5].title = status[RemoveDetector_isEnabled];
}

void CheckMemory(void);


// Update config memory field(used by k11 extension)
static void     SetConfigMemoryStatus(u32 status)
{
    *(vu32 *)PA_FROM_VA_PTR(0x1FF800F0) = status;
}

static u32      GetConfigMemoryEvent(void)
{
    return (*(vu32 *)PA_FROM_VA_PTR(0x1FF800F0)) & ~0xFFFF;
}

void    PLG__NotifyEvent(PLG_Event event, bool signal);

static bool WarningMessage(const char *fileName)
{
  bool ret = false;
  u16 posY;

  menuEnter();

  ClearScreenQuickly();

  do
  {
    Draw_Lock();

    Draw_DrawString(10, 10, COLOR_TITLE, "Rosalina");
    posY = Draw_DrawString(30, 30, COLOR_WHITE, "The 3gx will remove this file(or directory).");
    posY = Draw_DrawString(30, posY + 30, COLOR_WHITE, "Path:");
    posY = Draw_DrawString(30, posY + 15, COLOR_WHITE, fileName);
    posY = Draw_DrawString(30, 200, COLOR_WHITE, "Press A to continue, press B to block.");

    Draw_FlushFramebuffer();
    Draw_Unlock();

    u32 keys = waitComboWithTimeout(100000);
    if(keys & KEY_A)
    {
      ret = true;
      break;
    }
    else if(keys & KEY_B)
    {
      ret = false;
      break;
    }
  } while (1);

  menuLeave();

  return ret;
}

void     PluginLoader__HandleCommands(void *_ctx)
{
    (void)_ctx;

    u32    *cmdbuf = getThreadCommandBuffer();
    PluginLoaderContext *ctx = &PluginLoaderCtx;

    switch (cmdbuf[0] >> 16)
    {
        case 1: // Load plugin
        {
            if (cmdbuf[0] != IPC_MakeHeader(1, 0, 2))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            ctx->plgEvent = PLG_OK;
            ctx->target = cmdbuf[2];

            if (ctx->isEnabled && TryToLoadPlugin(ctx->target))
            {
                if (!ctx->useUserLoadParameters && ctx->userLoadParameters.noFlash)
                    ctx->userLoadParameters.noFlash = false;
                else
                {
                    // A little flash to notify the user that the plugin is loaded
                    for (u32 i = 0; i < 64; i++)
                    {
                        REG32(0x10202204) = 0x01FF9933;
                        svcSleepThread(5000000);
                    }
                    REG32(0x10202204) = 0;
                }
                //if (!ctx->userLoadParameters.noIRPatch)
                //    IR__Patch();
                SetConfigMemoryStatus(PLG_CFG_RUNNING);
            }
            else
            {
                svcCloseHandle(ctx->target);
                ctx->target = 0;
            }

            cmdbuf[0] = IPC_MakeHeader(1, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 2: // Check if plugin loader is enabled
        {
            if (cmdbuf[0] != IPC_MakeHeader(2, 0, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            cmdbuf[0] = IPC_MakeHeader(2, 2, 0);
            cmdbuf[1] = 0;
            cmdbuf[2] = (u32)ctx->isEnabled;
            break;
        }

        case 3: // Enable / Disable plugin loader
        {
            if (cmdbuf[0] != IPC_MakeHeader(3, 1, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            if (cmdbuf[1] != ctx->isEnabled)
            {
                ctx->isEnabled = cmdbuf[1];
                RequestSaveSettings();
                PluginLoader__UpdateMenu();
            }

            cmdbuf[0] = IPC_MakeHeader(3, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 4: // Define next plugin load settings
        {
            if (cmdbuf[0] != IPC_MakeHeader(4, 2, 4))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            PluginLoadParameters *params = &ctx->userLoadParameters;

            ctx->useUserLoadParameters = true;
            params->noFlash = cmdbuf[1];
            params->lowTitleId = cmdbuf[2];
            strncpy(params->path, (const char *)cmdbuf[4], 255);
            memcpy(params->config, (void *)cmdbuf[6], 32 * sizeof(u32));

            cmdbuf[0] = IPC_MakeHeader(4, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 5: // Display menu
        {
            if (cmdbuf[0] != IPC_MakeHeader(5, 1, 8))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            u32 nbItems = cmdbuf[1];
            u32 states = cmdbuf[3];
            DisplayPluginMenu(cmdbuf);

            cmdbuf[0] = IPC_MakeHeader(5, 1, 2);
            cmdbuf[1] = 0;
            cmdbuf[2] = IPC_Desc_Buffer(nbItems, IPC_BUFFER_RW);
            cmdbuf[3] = states;
            break;
        }

        case 6: // Display message
        {
            if (cmdbuf[0] != IPC_MakeHeader(6, 0, 4))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            const char *title = (const char *)cmdbuf[2];
            const char *body = (const char *)cmdbuf[4];

            DispMessage(title, body);

            cmdbuf[0] = IPC_MakeHeader(6, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 7: // Display error message
        {
            if (cmdbuf[0] != IPC_MakeHeader(7, 1, 4))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            const char *title = (const char *)cmdbuf[3];
            const char *body = (const char *)cmdbuf[5];

            DispErrMessage(title, body, cmdbuf[1]);

            cmdbuf[0] = IPC_MakeHeader(7, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 8: // Get PLGLDR Version
        {
            if (cmdbuf[0] != IPC_MakeHeader(8, 0, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            cmdbuf[0] = IPC_MakeHeader(8, 2, 0);
            cmdbuf[1] = 0;
            cmdbuf[2] = PLGLDR_VERSION;
            break;
        }

        case 9: // Get the arbiter (events)
        {
            if (cmdbuf[0] != IPC_MakeHeader(9, 0, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            cmdbuf[0] = IPC_MakeHeader(9, 1, 2);
            cmdbuf[1] = 0;
            cmdbuf[2] = IPC_Desc_SharedHandles(1);
            cmdbuf[3] = ctx->arbiter;
            break;
        }

        case 10: // Get plugin path
        {
            if (cmdbuf[0] != IPC_MakeHeader(10, 0, 2))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }

            char *path = (char *)cmdbuf[2];
            strncpy(path, ctx->pluginPath, 255);

            cmdbuf[0] = IPC_MakeHeader(10, 1, 2);
            cmdbuf[1] = 0;
            cmdbuf[2] = IPC_Desc_Buffer(255, IPC_BUFFER_RW);
            cmdbuf[3] = (u32)path;

            break;
        }

        case 11: // Set rosalina menu block
        {
            if (cmdbuf[0] != IPC_MakeHeader(11, 1, 0))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }
            
            g_blockMenuOpen = cmdbuf[1];
            
            cmdbuf[0] = IPC_MakeHeader(11, 1, 0);
            cmdbuf[1] = 0;
            break;
        }

        case 12: // Set swap settings
        {
            if (cmdbuf[0] != IPC_MakeHeader(12, 2, 4))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }
            cmdbuf[0] = IPC_MakeHeader(12, 1, 0);
            MemoryBlock__ResetSwapSettings();
            if (!cmdbuf[1] || !cmdbuf[2]) {
                cmdbuf[1] = MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, RD_INVALID_ADDRESS);
                break;
            }

            u32* remoteSavePhysAddr = (u32*)(cmdbuf[1] | (1 << 31));
            u32* remoteLoadPhysAddr = (u32*)(cmdbuf[2] | (1 << 31));

            Result ret = MemoryBlock__SetSwapSettings(remoteSavePhysAddr, false, (u32*)cmdbuf[4]);
            if (!ret) ret = MemoryBlock__SetSwapSettings(remoteLoadPhysAddr, true, (u32*)cmdbuf[4]);

            if (ret) {
                cmdbuf[1] = MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, RD_TOO_LARGE);
                MemoryBlock__ResetSwapSettings();
                break;
            }

            ctx->isSwapFunctionset = true;

            if (((char*)cmdbuf[6])[0] != '\0') strncpy(g_swapFileName, (char*)cmdbuf[6], 255);

            svcInvalidateEntireInstructionCache(); // Could use the range one

            cmdbuf[1] = 0;
            break;
        }

        case 13: // Set plugin exe load func
        {
            if (cmdbuf[0] != IPC_MakeHeader(13, 1, 2))
            {
                error(cmdbuf, 0xD9001830);
                break;
            }
            cmdbuf[0] = IPC_MakeHeader(13, 1, 0);
            Reset_3gx_LoadParams();
            if (!cmdbuf[1]) {
                cmdbuf[1] = MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, RD_INVALID_ADDRESS);
                break;
            }

            u32* remoteLoadExeFuncAddr = (u32*)(cmdbuf[1] | (1 << 31));
            Result ret = Set_3gx_LoadParams(remoteLoadExeFuncAddr, (u32*)cmdbuf[3]);
            if (ret)
            {
                cmdbuf[1] = MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, RD_TOO_LARGE);
                Reset_3gx_LoadParams();
                break;
            }
            
            ctx->isExeLoadFunctionset = true;

            svcInvalidateEntireInstructionCache(); // Could use the range one

            cmdbuf[1] = 0;
            break;
        }

        case 14: // Display warning message
        {
            if(cmdbuf[4] == PATH_UTF16 && cmdbuf[7] != 0)
            {
              u8          fileName[256]; // For UTF-8 file name
              char        textBuf[256];
              u32         header = cmdbuf[1];
              FS_Archive  archive;
              FS_Path     path;
              Result      ret = 0;
              bool        removeFile = false;

              // Clear buffers
              memset(fileName, 0, 256);
              memset(textBuf, 0, 256);

              // Convert the file name Utf16 to Utf8
              u32 u16NameAddr = ((u32)ctx->memblock.memblock + ctx->header.exeSize) + (cmdbuf[7] - ctx->header.heapVA);
              path = (FS_Path){PATH_UTF16, cmdbuf[5], (void *)u16NameAddr};
              utf16_to_utf8((u8 *)fileName, (u16 *)u16NameAddr, cmdbuf[5]);

              // Ignore files removed by CTRPF system
              sprintf(textBuf, "/cheats/%016llX.txt", g_titleId);
              if(strncmp(textBuf, (char *)fileName, strlen(textBuf)) == 0)
                removeFile = true;

              // Ignore files in the working directory
              sprintf(textBuf, "/luma/plugins/%016llX/", g_titleId);
              if(strncmp(textBuf, (char *)fileName, strlen(textBuf)) == 0)
                removeFile = true;
    
              // Display warning message
              if(!removeFile) 
                removeFile = WarningMessage((char *)fileName);

              // Remove the file
              if(removeFile)
              {
                if(R_SUCCEEDED(ret = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
                {
                  if(header == IPC_MakeHeader(0x804, 5, 2))
                  {
                    ret = FSUSER_DeleteFile(archive, path);
                  }
                  else
                  {
                    ret = FSUSER_DeleteDirectoryRecursively(archive, path);
                  }
                }
              }
            }
            
            break;
        }

        case 15:
        {
            cmdbuf[0] = IPC_MakeHeader(8, 2, 0);
            cmdbuf[1] = 0;
            cmdbuf[2] = RemoveDetector_isRunning;
            break;
        }

        default: // Unknown command
        {
            error(cmdbuf, 0xD900182F);
            break;
        }
    }

    if (ctx->error.message)
    {
        PluginLoader__Error(ctx->error.message, ctx->error.code);
        ctx->error.message = NULL;
        ctx->error.code = 0;
    }
}

static bool     ThreadPredicate(u32 *kthread)
{
    // Check if the thread is part of the plugin
    u32 *tls = (u32 *)kthread[0x26];

    return *tls == THREADVARS_MAGIC;
}

static void     __strex__(s32 *addr, s32 val)
{
    do
        __ldrex(addr);
    while (__strex(addr, val));
}

void    PLG__NotifyEvent(PLG_Event event, bool signal)
{
    if (!PluginLoaderCtx.plgEventPA) return;

    __strex__(PluginLoaderCtx.plgEventPA, event);
    if (signal)
        svcArbitrateAddress(PluginLoaderCtx.arbiter, (u32)PluginLoaderCtx.plgEventPA, ARBITRATION_SIGNAL, 1, 0);
}

void    PLG__WaitForReply(void)
{
    __strex__(PluginLoaderCtx.plgReplyPA, PLG_WAIT);
    svcArbitrateAddress(PluginLoaderCtx.arbiter, (u32)PluginLoaderCtx.plgReplyPA, ARBITRATION_WAIT_IF_LESS_THAN_TIMEOUT, PLG_OK, 10000000000ULL);
}

static void WaitForProcessTerminated(void *arg)
{
    (void)arg;
    PluginLoaderContext *ctx = &PluginLoaderCtx;

    // Unmap plugin's memory before closing the process
    if (!ctx->pluginIsSwapped) {
        MemoryBlock__UnmountFromProcess();
        MemoryBlock__Free();
    }
    // Terminate process
    svcCloseHandle(ctx->target);
    // Reset plugin loader state
    SetConfigMemoryStatus(PLG_CFG_NONE);
    ctx->pluginIsSwapped = false;
    ctx->target = 0;
    ctx->isExeLoadFunctionset = false;
    ctx->isSwapFunctionset = false;
    g_blockMenuOpen = 0;
    MemoryBlock__ResetSwapSettings();
    //if (!ctx->userLoadParameters.noIRPatch)
    //    IR__Unpatch();
}

void    PluginLoader__HandleKernelEvent(u32 notifId)
{
    (void)notifId;
    PluginLoaderContext *ctx = &PluginLoaderCtx;
    u32 event = GetConfigMemoryEvent();

    if (event == PLG_CFG_EXIT_EVENT)
    {
        if (!ctx->pluginIsSwapped)
        {
            // Signal the plugin that the game is exiting
            PLG__NotifyEvent(PLG_ABOUT_TO_EXIT, false);
            // Wait for plugin reply
            PLG__WaitForReply();
        }
        // Start a task to wait for process to be terminated
        TaskRunner_RunTask(WaitForProcessTerminated, NULL, 0);
    }
    else if (event == PLG_CFG_SWAP_EVENT)
    {
        if (ctx->pluginIsSwapped)
        {
            // Reload data from swap file
            MemoryBlock__IsReady();
            MemoryBlock__FromSwapFile();
            MemoryBlock__MountInProcess();
            // Unlock plugin threads
            svcControlProcess(ctx->target, PROCESSOP_SCHEDULE_THREADS, 0, (u32)ThreadPredicate);
            // Resume plugin execution
            PLG__NotifyEvent(PLG_OK, true);
            SetConfigMemoryStatus(PLG_CFG_RUNNING);
        }
        else
        {
            if(RemoveDetector_isEnabled)
              RemoveDetector_isRunning = false;
            // Signal plugin that it's about to be swapped
            PLG__NotifyEvent(PLG_ABOUT_TO_SWAP, false);
            // Wait for plugin reply
            PLG__WaitForReply();
            // Lock plugin threads
            svcControlProcess(ctx->target, PROCESSOP_SCHEDULE_THREADS, 1, (u32)ThreadPredicate);
            // Put data into file and release memory
            MemoryBlock__UnmountFromProcess();
            MemoryBlock__ToSwapFile();
            MemoryBlock__Free();
            SetConfigMemoryStatus(PLG_CFG_SWAPPED);
        }
        ctx->pluginIsSwapped = !ctx->pluginIsSwapped;
    }
    srvPublishToSubscriber(0x1002, 0);
}
