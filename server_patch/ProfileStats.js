// ProfileStats -- real race / pursuit statistics for PlayerProfile.gfx and the profile gadget.
// Drop this file into <server>\src\routes\ (routes are loaded automatically) and restart the server.
//
// The server already receives every event result (/event/arbitration, /event/bust, /event/abort) and the
// freeroam pursuits of NFSWorldPursuitProbe (/pursuitprobe/reward). This module listens to them and keeps
// the statistics of each driver in drivers\driverN\ProfileStats.txt (one "key=value" per line):
//   raw.*  counters used to update the statistics (do not edit while the server is running)
//   S.*    fields of com.ea.gameapi.data.ProfileStatistics   (races: overall / single player / multiplayer)
//   P.*    fields of com.ea.gameapi.data.ProfilePursuitStats (pursuits)
//   R.*    fields of com.ea.gameapi.data.ProfileRecordStats  (records)
// NFSWProfileStats.asi reads that file and hands it to the profile screens (Request "NFSWGetProfileStats").
// Nothing here changes rewards or the event flow: the original handlers run first and untouched.
const express = require("express");
const app = express.Router();
const fs = require("fs");
const path = require("path");
const log = require("../utils/log");
const xmlParser = require("../utils/xmlParser");
const personaManager = require("../services/personaManager");
const carManager = require("../services/carManager");
const eventManager = require("../services/eventManager");
const powerupManager = require("../services/powerupManager");
const allEvents = require("../../config/Assets/events.json");

const STATS_FILE = "ProfileStats.txt";
const MIN_ABORT_MS = 5000;       // an abort shorter than this (quit while loading) is not counted as a race
const MODE = { 4: "circuit", 9: "sprint", 19: "drag", 12: "pursuit", 24: "teamescape", 22: "meeting" };

// ---------------------------------------------------------------- file of the driver
async function driverDir(personaId) {
    const p = await personaManager.getPersonaById(`${personaId}`);
    return p.success ? p.data.driverDirectory : null;
}
async function loadRaw(personaId) {
    const dir = await driverDir(personaId);
    const raw = {};
    if (!dir) return { dir: null, raw };
    const file = path.join(dir, STATS_FILE);
    if (fs.existsSync(file)) {
        for (const line of fs.readFileSync(file).toString().split(/\r?\n/)) {
            const m = /^raw\.([A-Za-z0-9_]+)=(.*)$/.exec(line);
            if (m) raw[m[1]] = Number(m[2]) || 0;
        }
    }
    return { dir, raw };
}

// ---------------------------------------------------------------- formatting
const ms2time = (ms) => {
    if (!(ms > 0)) return "0:00.00";
    const cs = Math.floor(ms / 10), m = Math.floor(cs / 6000), s = Math.floor(cs / 100) % 60, c = cs % 100;
    return `${m}:${s < 10 ? "0" : ""}${s}.${c < 10 ? "0" : ""}${c}`;
};
// TopSpeed of the arbitration packets is in m/s
const kmh = (ms) => Math.round((Number(ms) || 0) * 3.6);
const speed = (ms) => `${kmh(ms)} KM/H`;
const pct = (a, b) => (b > 0 ? Math.round(a * 100 / b) : 0);
const avg = (sum, n) => (n > 0 ? Math.round(sum * 100 / n) / 100 : 0);
const clean = (v) => `${v}`.replace(/[\r\n=]/g, " ");

async function save(personaId, dir, r) {
    let cars = r.carsOwned || 0;
    try {
        const slots = await carManager.getCarslots(personaId);
        if (slots.success) {
            const parsed = await xmlParser.parseXML(slots.data.carslotsData);
            cars = parsed?.CarSlotInfoTrans?.CarsOwnedByPersona?.[0]?.OwnedCarTrans?.length || 0;
            r.carsOwned = cars;
        }
    } catch { }

    const S = {}, P = {}, R = {};
    const g = (k) => r[k] || 0;
    // races
    for (const t of ["sp", "mp"]) {
        r[`${t}Lost`] = Math.max(0, g(`${t}Races`) - g(`${t}Wins`));
    }
    const races = g("spRaces") + g("mpRaces"), wins = g("spWins") + g("mpWins");
    S.personaId = personaId;
    S.singlePlayerTotalRaces = g("spRaces");
    S.singlePlayerEventsWon = g("spWins");
    S.singlePlayerEventsLost = g("spLost");
    S.singlePlayerEventWinPercentage = pct(g("spWins"), g("spRaces"));
    S.singlePlayerEventWinStreak = g("spBestStreak");
    S.singlePlayerAvgPlacing = avg(g("spPlaceSum"), g("spFinished"));
    S.singlePlayerMoneyWon = g("spCash");
    S.rankedTotalRaces = g("mpRaces");
    S.totalRankedEvents = g("mpRaces");
    S.rankedWins = g("mpWins");
    S.rankedLosses = g("mpLost");
    S.rankedWinPercentage = pct(g("mpWins"), g("mpRaces"));
    S.rankedAvgPlacing = avg(g("mpPlaceSum"), g("mpFinished"));
    S.rankedWinStreak = g("mpStreak");
    S.rankedBestWinStreak = g("mpBestStreak");
    S.rankedDNFCount = g("mpDnf");
    S.rankedMoneyWon = g("mpCash");
    S.overallTotalRaces = races;
    S.totalRacesWon = wins;
    S.totalRacesLost = Math.max(0, races - wins);
    S.totalWinPercentage = pct(wins, races);
    S.bestWinStreak = g("bestStreak");
    S.overallWinningStreak = g("bestStreak");
    S.overallDNFCount = g("spDnf") + g("mpDnf");
    S.DNFPercentage = pct(S.overallDNFCount, races);
    S.averagePlacing = avg(g("spPlaceSum") + g("mpPlaceSum"), g("spFinished") + g("mpFinished"));
    S.totalMoneyWon = g("spCash") + g("mpCash");
    S.totalCarsOwned = cars;
    S.disconnectPercentage = 0;
    S.numberOfPlayerCarCollisions = 0;
    S.numberOfPowerupsUsed = g("powerups");
    S.perfectStartNumSuccess = g("perfectStarts");
    S.perfectStartNumFail = Math.max(0, g("startsCounted") - g("perfectStarts"));
    S.numberOfDuels = 0;
    S.numberOfHonorChallenges = 0;
    S.numberOfHonorWins = 0;
    // pursuits ("single" = pursuit events + freeroam pursuits, "coop" = team escape)
    P.personaId = personaId;
    P.singlePursuitCount = g("pSingle");
    P.PursuitCount = g("pSingle");
    P.coopPursuitCount = g("pCoop");
    P.overallPursuitCount = g("pSingle") + g("pCoop");
    P.singlePursuitEvadeCount = g("pSingleEvaded");
    P.coopPursuitEvadeCount = g("pCoopEvaded");
    P.pursuitEvadeCount = g("pSingleEvaded") + g("pCoopEvaded");
    P.bustedCount = g("pBusted");
    P.pursuitAverageEvaded = pct(P.pursuitEvadeCount, P.overallPursuitCount);
    P.longestPursuit = ms2time(g("pSingleLongest"));
    P.shortestPursuit = ms2time(g("pSingleShortest"));
    P.cooplongestPursuit = ms2time(g("pCoopLongest"));
    P.overallLongestPursuit = ms2time(Math.max(g("pSingleLongest"), g("pCoopLongest")));
    const shortest = [g("pSingleShortest"), g("pCoopShortest")].filter(v => v > 0);
    P.overallShortestPursuit = ms2time(shortest.length ? Math.min(...shortest) : 0);
    P.copsDisabledSingle = g("pMaxDisabled");
    P.copsDisabledTotal = g("pDisabled");
    P.copsRammedSingle = g("pMaxRammed");
    P.copsRammedTotal = g("pRammed");
    P.spikeStripDodgedSingle = g("pMaxSpikes");
    P.spikeStripDodgedTotal = g("pSpikes");
    P.roadBlocksDodgedSingle = g("pMaxRoadblocks");
    P.roadBlocksDodgedTotal = g("pRoadblocks");
    P.infractionsSingle = g("pMaxInfractions");
    P.infractionsTotal = g("pInfractions");
    P.costToStateSingle = g("pMaxCost");
    P.costToStateTotal = g("pSingleCost");
    P.coopCostToStateTotal = g("pCoopCost");
    P.overallCostToStateTotal = g("pSingleCost") + g("pCoopCost");
    P.bountySingle = g("pMaxBounty");
    P.overallBountySingle = g("pSingleBounty");
    P.bountyTotal = g("pSingleBounty");
    P.coopBountyTotal = g("pCoopBounty");
    P.overallBountyTotal = g("pSingleBounty") + g("pCoopBounty");
    P.highestClockedSpeed = kmh(g("pTopSpeed"));
    P.totalClockedSpeed = kmh(g("pTopSpeedSum"));
    P.highestHeat = g("pMaxHeat") ? `${Math.floor(g("pMaxHeat"))}` : "0";
    P.singleCopsDeployed = g("pSingleMaxDeployed");
    P.coopCopsDeployed = g("pCoopMaxDeployed");
    P.overallCopsDeployed = Math.max(g("pSingleMaxDeployed"), g("pCoopMaxDeployed"));
    P.singleCopsEvaded = 0;
    P.coopCopsEvaded = 0;
    P.overallCopsEvaded = 0;
    P.singleSpikesDodged = g("pSingleSpikes");
    P.coopSpikesDodged = g("pCoopSpikes");
    P.overallSpikesDodged = g("pSpikes");
    P.singleRoadBlocksDodged = g("pSingleRoadblocks");
    P.coopRoadBlocksDodged = g("pCoopRoadblocks");
    P.overallRoadBlocksDodged = g("pRoadblocks");
    P.coopFavoritePartner = "-";
    // records
    R.personaId = personaId;
    R.bestOverallTopSpeed = speed(Math.max(g("spTopSpeed"), g("mpTopSpeed")));
    R.bestSinglePlayerTopSpeed = speed(g("spTopSpeed"));
    R.bestRankedTopSpeed = speed(g("mpTopSpeed"));
    const bestTimes = [g("sprintBest"), g("circuitBest"), g("dragBest")].filter(v => v > 0);
    R.bestOverallTime = ms2time(bestTimes.length ? Math.min(...bestTimes) : 0);
    R.bestOverallLapTime = ms2time(g("circuitBestLap"));
    R.bestSprintEventTime = ms2time(g("sprintBest"));
    R.bestSprintEventTopSpeed = kmh(g("sprintTopSpeed"));
    R.bestSprintEventPlayCount = g("sprintPlays");
    R.bestCircuitEventTime = ms2time(g("circuitBest"));
    R.bestCircuitEventLapTime = ms2time(g("circuitBestLap"));
    R.bestCircuitEventTopSpeed = kmh(g("circuitTopSpeed"));
    R.bestCircuitEventPlayCount = g("circuitPlays");
    R.bestCheckpointEventTime = ms2time(g("dragBest"));       // drag has no field of its own in the beta records
    R.bestCheckpointEventTopSpeed = kmh(g("dragTopSpeed"));
    R.bestCheckpointEventPlayCount = g("dragPlays");
    for (const k of ["bestSpeedTrapEventScore", "bestSpeedTrapEventCumulativeSpeed", "bestSpeedTrapEventTopSpeed", "bestSpeedTrapEventPlayCount",
        "bestDriftEventScore", "bestDriftEventLapScore", "bestDriftEventChainScore", "bestDriftEventLength", "bestDriftEventTopSpeed", "bestDriftEventPlayCount",
        "bestCanyonEventScore", "bestCanyonEventKOWins", "bestCanyonEventWins", "bestCanyonEventPlayCount"]) R[k] = 0;
    R.bestCanyonEventLapTime = "0:00.00";

    const lines = [`# NFSW ProfileStats - driver ${personaId} - ${new Date().toISOString()}`];
    for (const k of Object.keys(r).sort()) lines.push(`raw.${k}=${r[k]}`);
    for (const [pfx, obj] of [["S", S], ["P", P], ["R", R]]) for (const k of Object.keys(obj)) lines.push(`${pfx}.${k}=${clean(obj[k])}`);
    fs.writeFileSync(path.join(dir, STATS_FILE), lines.join("\r\n") + "\r\n");
}

async function update(personaId, fn) {
    try {
        const { dir, raw } = await loadRaw(personaId);
        if (!dir) return;
        fn(raw);
        await save(personaId, dir, raw);
    } catch (e) {
        log.error("ProfileStats", e?.stack || e);
    }
}
const inc = (r, k, v = 1) => { r[k] = (r[k] || 0) + v; };
const max = (r, k, v) => { if (v > (r[k] || 0)) r[k] = v; };
const min = (r, k, v) => { if (v > 0 && (!(r[k] > 0) || v < r[k])) r[k] = v; };

// ---------------------------------------------------------------- event sessions
const sessions = {};          // eventSessionId -> { eventId, mp }
let multiplayerNext = false;  // set by Event.js' "Launching the detected multiplayer event" just before the session is created

if (!log.__profileStatsWrapped) {
    const originalGame = log.game;
    log.game = (...args) => {
        if (`${args[0]}`.startsWith("Launching the detected multiplayer event")) multiplayerNext = true;
        return originalGame(...args);
    };
    log.__profileStatsWrapped = true;
}

if (!eventManager.__profileStatsWrapped) {
    const originalCreate = eventManager.createSinglePlayerEvent;
    eventManager.createSinglePlayerEvent = async (personaId, eventId, ...rest) => {
        const result = await originalCreate(personaId, eventId, ...rest);
        if (result?.success) sessions[result.data.sessionId] = { eventId: `${eventId}`, mp: multiplayerNext };
        multiplayerNext = false;
        return result;
    };

    const originalFinish = eventManager.finishEvent;
    eventManager.finishEvent = async (personaId, eventSessionId, arbitrationBody, eventAction, ...rest) => {
        const session = sessions[eventSessionId];
        const result = await originalFinish(personaId, eventSessionId, arbitrationBody, eventAction, ...rest);
        if (result?.success && session) {
            delete sessions[eventSessionId];
            try { await record(personaId, session, arbitrationBody, eventAction, result.data); }
            catch (e) { log.error("ProfileStats", e?.stack || e); }
        }
        return result;
    };
    eventManager.__profileStatsWrapped = true;
}

if (!powerupManager.__profileStatsWrapped) {
    const originalActivate = powerupManager.activatePowerup;
    powerupManager.activatePowerup = async (...args) => {
        const result = await originalActivate(...args);
        const active = personaManager.getActivePersona();
        if (active.success) await update(active.data.personaId, r => inc(r, "powerups"));
        return result;
    };
    powerupManager.__profileStatsWrapped = true;
}

async function record(personaId, session, arbitrationBody, eventAction, resultData) {
    let body = await xmlParser.parseXML(arbitrationBody);
    const root = xmlParser.getRootName(body);
    if (!root) return;
    body = body[root];
    const kind = root.split("Arbitration")[0];                 // Route, Drag, Pursuit, TeamEscape
    const num = (k) => Number(body[k]?.[0]) || 0;
    const eventData = allEvents.find(e => `${e.ID}` == session.eventId);
    const mode = MODE[eventData?.eventModeId] || kind.toLowerCase();
    const resultRoot = resultData ? Object.keys(resultData)[0] : null;
    const tokens = parseInt(resultData?.[resultRoot]?.Accolades?.[0]?.FinalRewards?.[0]?.Tokens?.[0]) || 0;
    const duration = num("EventDurationInMilliseconds");
    const finishReason = num("FinishReason");
    const t = session.mp ? "mp" : "sp";

    if (kind == "Pursuit" || kind == "TeamEscape") {
        if (eventAction == "abort") return;
        const coop = kind == "TeamEscape";
        const busted = eventAction == "bust";
        await update(personaId, r => {
            const c = coop ? "pCoop" : "pSingle";
            inc(r, c);
            if (busted) inc(r, "pBusted"); else inc(r, `${c}Evaded`);
            if (!busted && duration > 0) { max(r, `${c}Longest`, duration); min(r, `${c}Shortest`, duration); }
            const deployed = num("CopsDeployed"), disabled = num("CopsDisabled"), rammed = num("CopsRammed");
            const spikes = num("SpikeStripsDodged"), roadblocks = num("RoadBlocksDodged"), infractions = num("Infractions");
            const cost = num("CostToState");
            max(r, `${c}MaxDeployed`, deployed);
            inc(r, "pDisabled", disabled); max(r, "pMaxDisabled", disabled);
            inc(r, "pRammed", rammed); max(r, "pMaxRammed", rammed);
            inc(r, "pSpikes", spikes); max(r, "pMaxSpikes", spikes); inc(r, `${c}Spikes`, spikes);
            inc(r, "pRoadblocks", roadblocks); max(r, "pMaxRoadblocks", roadblocks); inc(r, `${c}Roadblocks`, roadblocks);
            inc(r, "pInfractions", infractions); max(r, "pMaxInfractions", infractions);
            inc(r, `${c}Cost`, cost); max(r, "pMaxCost", cost);
            if (!busted) { inc(r, `${c}Bounty`, tokens); max(r, "pMaxBounty", tokens); }
            max(r, "pTopSpeed", num("TopSpeed")); inc(r, "pTopSpeedSum", num("TopSpeed"));
            max(r, "pMaxHeat", num("Heat"));
        });
        log.game(`[ProfileStats] ${coop ? "team escape" : "pursuit"} ${busted ? "busted" : "evaded"} recorded`);
        return;
    }

    // races (Route = sprint/circuit, Drag)
    const finished = eventAction == "arbitration" && (finishReason == 22 || finishReason == 518);
    if (eventAction == "abort" && duration < MIN_ABORT_MS) return;
    if (eventAction != "arbitration" && eventAction != "abort") return;
    const rank = num("Rank");
    const won = finished && rank == 1;
    await update(personaId, r => {
        inc(r, `${t}Races`);
        if (finished) { inc(r, `${t}Finished`); inc(r, `${t}PlaceSum`, rank > 0 ? rank : 1); }
        else inc(r, `${t}Dnf`);
        if (won) {
            inc(r, `${t}Wins`); inc(r, `${t}Streak`); max(r, `${t}BestStreak`, r[`${t}Streak`]);
            inc(r, "streak"); max(r, "bestStreak", r.streak);
        } else { r[`${t}Streak`] = 0; r.streak = 0; }
        inc(r, `${t}Cash`, tokens);
        const top = num("TopSpeed");
        max(r, `${t}TopSpeed`, top);
        if (body.PerfectStart) { inc(r, "startsCounted"); if (body.PerfectStart[0] == "1" || body.PerfectStart[0] == "true") inc(r, "perfectStarts"); }
        if (mode == "sprint" || mode == "circuit" || mode == "drag") {
            inc(r, `${mode}Plays`);
            max(r, `${mode}TopSpeed`, top);
            if (finished) min(r, `${mode}Best`, duration);
            if (mode == "circuit") min(r, "circuitBestLap", num("BestLapDurationInMilliseconds"));
        }
    });
    log.game(`[ProfileStats] ${t == "mp" ? "multiplayer" : "single player"} ${mode} ${finished ? `finished ${rank}°` : "DNF"} recorded`);
}

// ---------------------------------------------------------------- freeroam pursuits of NFSWorldPursuitProbe
// This router is mounted before PursuitProbe.js (alphabetical order): record, then let the real route answer.
// Besides busted/cash/heat, recent probe builds also send the pursuit statistics of the results screen:
//   duration (ms), deployed, disabled, rammed, cost, roadblocks, spikes, infractions, topspeed (m/s)
//   (all optional: older builds only send the outcome)
app.post("/pursuitprobe/reward", async (req, res, next) => {
    const active = personaManager.getActivePersona();
    if (active.success) {
        const q = (k) => { const v = Number(req.query[k]); return Number.isFinite(v) && v > 0 ? v : 0; };
        const busted = req.query.busted == "1";
        const cash = Math.trunc(q("cash"));
        const duration = Math.trunc(q("duration"));
        const deployed = Math.trunc(q("deployed")), disabled = Math.trunc(q("disabled")), rammed = Math.trunc(q("rammed"));
        const cost = Math.trunc(q("cost")), roadblocks = Math.trunc(q("roadblocks"));
        const spikes = Math.trunc(q("spikes")), infractions = Math.trunc(q("infractions")), topSpeed = q("topspeed");
        await update(active.data.personaId, r => {
            inc(r, "pSingle");
            if (busted) inc(r, "pBusted");
            else {
                inc(r, "pSingleEvaded"); inc(r, "pSingleBounty", cash); max(r, "pMaxBounty", cash);
                if (duration > 0) { max(r, "pSingleLongest", duration); min(r, "pSingleShortest", duration); }
            }
            max(r, "pSingleMaxDeployed", deployed);
            inc(r, "pDisabled", disabled); max(r, "pMaxDisabled", disabled);
            inc(r, "pRammed", rammed); max(r, "pMaxRammed", rammed);
            inc(r, "pRoadblocks", roadblocks); max(r, "pMaxRoadblocks", roadblocks); inc(r, "pSingleRoadblocks", roadblocks);
            inc(r, "pSingleCost", cost); max(r, "pMaxCost", cost);
            inc(r, "pSpikes", spikes); max(r, "pMaxSpikes", spikes); inc(r, "pSingleSpikes", spikes);
            inc(r, "pInfractions", infractions); max(r, "pMaxInfractions", infractions);
            max(r, "pTopSpeed", topSpeed); inc(r, "pTopSpeedSum", topSpeed);
            max(r, "pMaxHeat", q("heat"));
        });
        log.game(`[ProfileStats] freeroam pursuit ${busted ? "busted" : "evaded"} recorded${duration ? "" : " (outcome only: probe without statistics)"}`);
    }
    next();
});

module.exports = app;
