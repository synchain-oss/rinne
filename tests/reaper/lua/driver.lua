-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- REAPER 烟测的 Lua 驱动。runner 把任务写成 job.lua（return { … }），经环境变量 RINNE_REAPER_JOB 交给本脚本；
-- 脚本做完把「键=值」写进 job.result，最后一行 done=1，然后退出 REAPER。出错也写 error=… 再退出；
-- 只有 hang 一步故意不退出（超时与杀进程反例用）。需要等的地方都用协程让出，不阻塞 REAPER 的消息循环。
--   probe       读设备块长 / 采样率，读两个插件的上报延迟（pdc 与 chain_pdc_actual）
--   instances   反复加减插件实例，在预热结束和循环结束时落标记文件，runner 在这两处取进程的句柄与内存
--   savereopen  打开工程、存成新文件、再打开新文件读一遍状态；runner 之后离线渲染新文件
--   probeplay   探针稳态格：经隐藏参数开记录窗口，实时播放，停走带触发探针的自动 Ping，等窗口到时
--   probeexport 探针导出格：经隐藏参数开记录窗口，按工程里的渲染设置渲染一次，等窗口到时

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

-- ---- 第二段：探针 ----
-- 探针的窗口按钮 Lua 碰不到，只靠三个隐藏参数开记录窗口。参数按名字找（不假设下标）；AudioParameterInt 的取值 0..上限按 v / 上限
-- 归一化，上限从参数自己读（设到 1.0 看显示的整数），每次写入后核对显示值。三项合规且与上一次不同时探针开一个窗口，
-- 所以先把 case 与 variant 写好，最后两端一起写 auto_record_s；读 auto_record_s 的上限时 case 还是 0（不合规，不会开窗口）。

local function waitUntil(t)
    while reaper.time_precise() < t do coroutine.yield() end
end

local function findParam(track, fx, name)
    for p = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
        local _, pn = reaper.TrackFX_GetParamName(track, fx, p, "")
        if pn == name then return p end
    end
    error("第 " .. fx .. " 个插件上找不到参数 " .. name)
end

local function paramText(track, fx, p)
    local _, txt = reaper.TrackFX_GetFormattedParamValue(track, fx, p, "")
    return txt
end

local function paramMax(track, fx, p)
    reaper.TrackFX_SetParamNormalized(track, fx, p, 1.0)
    local top = tonumber(paramText(track, fx, p))
    reaper.TrackFX_SetParamNormalized(track, fx, p, 0.0)
    if not top or top < 1 then error("读不出第 " .. fx .. " 个插件第 " .. p .. " 个参数的上限") end
    return top
end

local function setInt(track, fx, p, top, value)
    reaper.TrackFX_SetParamNormalized(track, fx, p, value / top)
    local txt = paramText(track, fx, p)
    if tonumber(txt) ~= value then error("第 " .. fx .. " 个插件第 " .. p .. " 个参数写入后显示 " .. tostring(txt) .. "，应为 " .. value) end
end

local function probeChain(track)
    if reaper.TrackFX_GetCount(track) ~= 2 then error("轨道上应有 2 个插件，实际 " .. reaper.TrackFX_GetCount(track)) end
    for i, want in ipairs({ "Rinne Probe Input", "Rinne Probe Output" }) do
        local _, name = reaper.TrackFX_GetFXName(track, i - 1, "")
        put("fx" .. (i - 1) .. "_name", name)
        if not string.find(name, want, 1, true) then error("第 " .. (i - 1) .. " 个插件不是 " .. want) end
    end
end

-- 两端都开窗口；返回窗口开始的时刻
local function armRecording(track)
    local records = {}
    for fx = 0, 1 do
        local pc, pv, pr = findParam(track, fx, "auto_case"), findParam(track, fx, "auto_variant"), findParam(track, fx, "auto_record_s")
        for _, p in ipairs({ pr, pc, pv }) do reaper.TrackFX_SetParamNormalized(track, fx, p, 0.0) end
        local topR = paramMax(track, fx, pr)
        local topC = paramMax(track, fx, pc)
        local topV = paramMax(track, fx, pv)
        put("fx" .. fx .. "_max", topC .. "," .. topV .. "," .. topR)
        if topC ~= job.caseMax or topV ~= job.variantMax then
            error("第 " .. fx .. " 个插件的 auto_case / auto_variant 上限是 " .. topC .. " / " .. topV .. "，规则表是 " .. job.caseMax .. " / " .. job.variantMax)
        end
        setInt(track, fx, pc, topC, job.autoCase)
        setInt(track, fx, pv, topV, job.autoVariant)
        records[#records + 1] = { fx = fx, p = pr, top = topR }
    end
    for _, r in ipairs(records) do setInt(track, r.fx, r.p, r.top, job.recordSeconds) end
    return reaper.time_precise(), records
end

local function disarm(track, records)
    for _, r in ipairs(records) do reaper.TrackFX_SetParamNormalized(track, r.fx, r.p, 0.0) end
end

-- 稳态格：开窗口、实时播放、停走带（探针在停走带时自动 Ping），等窗口到时再退出，让日志正常收尾
function steps.probeplay()
    openAndWait(job.project)
    local track = reaper.GetTrack(0, 0)
    probeChain(track)
    local armedAt, records = armRecording(track)
    wait(job.leadSeconds)
    reaper.OnPlayButton()
    wait(job.playSeconds)
    put("play_state", reaper.GetPlayState())
    put("play_pos", string.format("%.3f", reaper.GetPlayPosition()))
    reaper.OnStopButton()
    put("stopped_after", string.format("%.3f", reaper.time_precise() - armedAt))
    waitUntil(armedAt + job.recordSeconds + job.marginSeconds)
    disarm(track, records)
    wait(0.3)
end

-- 导出格：开窗口后按工程里的渲染设置渲染（离线或 1x 由工程决定），等窗口到时再退出
function steps.probeexport()
    openAndWait(job.project)
    local track = reaper.GetTrack(0, 0)
    probeChain(track)
    local armedAt, records = armRecording(track)
    wait(job.leadSeconds)
    local t0 = reaper.time_precise()
    reaper.Main_OnCommand(42230, 0) -- File: Render project, using the most recent render settings, auto-close render dialog
    put("render_seconds", string.format("%.3f", reaper.time_precise() - t0))
    put("rendered", reaper.file_exists(job.wav))
    waitUntil(armedAt + job.recordSeconds + job.marginSeconds)
    disarm(track, records)
    wait(0.3)
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
