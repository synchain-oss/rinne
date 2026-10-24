-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- REAPER 烟测的 Lua 驱动。runner 把任务写成 job.lua（return { … }），经环境变量 RINNE_REAPER_JOB 交给本脚本；
-- 脚本做完把「键=值」写进 job.result，最后一行 done=1，然后退出 REAPER。出错也写 error=… 再退出；
-- 只有 hang 一步故意不退出（超时与杀进程反例用）。需要等的地方都用协程让出，不阻塞 REAPER 的消息循环。
--   probe       读设备块长 / 采样率，读两个插件的上报延迟（pdc 与 chain_pdc_actual）
--   instances   反复加减插件实例，在预热结束和循环结束时落标记文件，runner 在这两处取进程的句柄与内存
--   savereopen  打开工程、存成新文件、再打开新文件读一遍状态；runner 之后离线渲染新文件

local jobPath = os.getenv("RINNE_REAPER_JOB")
local job = dofile(jobPath)
local lines = {}
local function put(key, value) lines[#lines + 1] = key .. "=" .. tostring(value) end

local function flushResult()
    local part = job.result .. ".part"
    local f = assert(io.open(part, "w"))
    f:write(table.concat(lines, "\n"), "\ndone=1\n")
    f:close()
    os.remove(job.result)
    assert(os.rename(part, job.result))
end

local function touch(path)
    local f = assert(io.open(path, "w"))
    f:write("1")
    f:close()
end

local function wait(seconds)
    local untilTime = reaper.time_precise() + seconds
    while reaper.time_precise() < untilTime do coroutine.yield() end
end

local function device(attr)
    for _ = 1, 50 do
        local ok, value = reaper.GetAudioDeviceInfo(attr, "")
        if ok then return value end
        wait(0.1)
    end
    return "unavailable"
end

local function openAndWait(path)
    reaper.Main_openProject("noprompt:" .. path)
    wait(1.0) -- 刚打开就开播会在 0.3 s 内被取消；读状态也等一下，让插件实例化完
end

local function putFx(prefix)
    local track = reaper.GetTrack(0, 0)
    local count = reaper.TrackFX_GetCount(track)
    put(prefix .. "fx_count", count)
    for i = 0, count - 1 do
        local _, name = reaper.TrackFX_GetFXName(track, i, "")
        local _, pdc = reaper.TrackFX_GetNamedConfigParm(track, i, "pdc")
        local _, actual = reaper.TrackFX_GetNamedConfigParm(track, i, "chain_pdc_actual")
        put(prefix .. "fx" .. i .. "_name", name)
        put(prefix .. "fx" .. i .. "_pdc", pdc)
        put(prefix .. "fx" .. i .. "_chain_pdc_actual", actual)
        put(prefix .. "fx" .. i .. "_bypassed", not reaper.TrackFX_GetEnabled(track, i))
    end
end

local steps = {}

function steps.probe()
    openAndWait(job.project)
    put("dev_bsize", device("BSIZE"))
    put("dev_srate", device("SRATE"))
    put("dev_mode", device("MODE"))
    put("project_srate", reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 0, false))
    putFx("")
end

function steps.instances()
    openAndWait(job.project)
    local made = 0
    for i = 1, job.cycles do
        reaper.InsertTrackAtIndex(reaper.CountTracks(0), false)
        local track = reaper.GetTrack(0, reaper.CountTracks(0) - 1)
        local a = reaper.TrackFX_AddByName(track, "Rinne Input", false, -1)
        local b = reaper.TrackFX_AddByName(track, "Rinne Output", false, -1)
        if a ~= 0 or b ~= 1 then error("第 " .. i .. " 轮插入失败：" .. tostring(a) .. "," .. tostring(b)) end
        if job.play then
            reaper.OnPlayButton()
            wait(0.25)
            reaper.OnStopButton()
        else
            wait(0.1)
        end
        reaper.DeleteTrack(track)
        made = made + 1
        if i == job.warmup then
            touch(job.warmMark)
            wait(0.8)
        end
    end
    put("cycles_done", made)
    put("tracks_left", reaper.CountTracks(0))
    touch(job.loopMark)
    wait(0.8)
end

function steps.savereopen()
    openAndWait(job.project)
    putFx("before_")
    reaper.Main_SaveProjectEx(0, job.saveAs, 0)
    wait(0.5)
    openAndWait(job.saveAs)
    putFx("after_")
    put("saved", reaper.file_exists(job.saveAs))
end

function steps.hang()
    while true do wait(0.5) end
end

local function run()
    local step = steps[job.step]
    if not step then error("没有这一步：" .. tostring(job.step)) end
    step()
end

local co = coroutine.create(function()
    local ok, err = pcall(run)
    if not ok then put("error", tostring(err)) end
    flushResult()
    reaper.Main_OnCommand(40004, 0) -- File: Quit REAPER（工程有改动时会弹存盘提示，runner 点「否」）
end)

local function pump()
    local ok, err = coroutine.resume(co)
    if not ok then
        put("error", "driver: " .. tostring(err))
        pcall(flushResult)
        reaper.Main_OnCommand(40004, 0)
    elseif coroutine.status(co) ~= "dead" then
        reaper.defer(pump)
    end
end

pump()
