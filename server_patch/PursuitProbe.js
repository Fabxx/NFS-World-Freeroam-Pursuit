// NFSWorldPursuitProbe -- rewards of a freeroam pursuit started by the ASI.
// Drop this file into <server>\src\routes\ (routes are loaded automatically) and restart the server.
// The ASI computes the rewards itself (event 385 formula) and only asks the server to save them:
//   POST /Engine.svc/pursuitprobe/reward?busted=0|1&cash=N&rep=N&heat=F&item=<entitlementTag>&qty=N
// Same effects as /event/arbitration and /event/bust for a real pursuit:
//   evaded: cash + REP (with level up), car heat = packet heat, durability -5, lucky draw item to inventory
//   busted: car heat = 1, durability -5
const express = require("express");
const app = express.Router();
const fs = require("fs");
const log = require("../utils/log");
const xmlParser = require("../utils/xmlParser");
const personaManager = require("../services/personaManager");
const carManager = require("../services/carManager");
const inventoryManager = require("../services/inventoryManager");
const rewardManager = require("../services/rewardManager");
const powerupManager = require("../services/powerupManager");

// Powerups used during an ASI pursuit. The game reports every powerup it fires to
// POST /Engine.svc/powerups/activated/<hash> (routes/Powerup.js -> powerupManager.activatePowerup):
// the call is wrapped here to count them between /pursuitprobe/powerups/reset and /pursuitprobe/powerups.
let usedPowerups = null;   // null = not counting
if (!powerupManager.__pursuitProbeWrapped) {
    const originalActivate = powerupManager.activatePowerup;
    powerupManager.activatePowerup = async (itemHash, ...rest) => {
        if (usedPowerups && ((typeof itemHash) == "string" || (typeof itemHash) == "number")) {
            const key = `${parseInt(itemHash) | 0}`;
            usedPowerups[key] = (usedPowerups[key] || 0) + 1;
        }
        return originalActivate(itemHash, ...rest);
    };
    powerupManager.__pursuitProbeWrapped = true;
}

app.post("/pursuitprobe/powerups/reset", (req, res) => {
    usedPowerups = {};
    res.status(200).end();
});

// "hash count" per line (signed 32-bit hash, as the game sends it)
app.get("/pursuitprobe/powerups", (req, res) => {
    const used = usedPowerups || {};
    const lines = Object.keys(used).map(k => `${k} ${used[k]}`);
    usedPowerups = null;
    if (lines.length) log.game(`[PursuitProbe] powerups used: ${lines.join(", ")}`);
    res.type("text/plain").send(lines.join("\n"));
});

app.post("/pursuitprobe/reward", async (req, res) => {
    const getActivePersona = personaManager.getActivePersona();
    if (!getActivePersona.success) return res.status(getActivePersona.error.status).send(getActivePersona.error.reason);
    const personaId = getActivePersona.data.personaId;

    const busted = req.query.busted == "1";
    const cash = parseInt(req.query.cash) || 0;
    const rep = parseInt(req.query.rep) || 0;
    let heat = Number(req.query.heat) || 1;
    if (busted || heat < 1) heat = 1;
    if (heat > 5) heat = 5;

    let result = { busted: busted, cash: 0, rep: 0, item: "", level: 0, leveledUp: false };

    if (!busted && (cash > 0 || rep > 0)) {
        const add = await personaManager.addCashAndRep(personaId, cash, rep);
        if (add.success) {
            result.cash = cash;
            result.rep = rep;
            result.level = add.data.newLevel;
            result.leveledUp = add.data.hasLeveledUp;
        }
    }

    const getCarslots = await carManager.getCarslots(personaId);
    if (getCarslots.success) {
        const parsed = await xmlParser.parseXML(getCarslots.data.carslotsData);
        const idx = parsed.CarSlotInfoTrans.DefaultOwnedCarIndex?.[0];
        const car = parsed.CarSlotInfoTrans.CarsOwnedByPersona?.[0]?.OwnedCarTrans?.[idx];
        if (car) {
            let durability = (parseInt(car.Durability?.[0]) || 0) - 5;
            if (durability < 0) durability = 0;
            car.Durability = [`${durability}`];
            car.Heat = [`${heat}`];
            fs.writeFileSync(getCarslots.data.carslotsPath, xmlParser.buildXML(parsed, { pretty: true }));
        }
    }

    const tag = ((typeof req.query.item) == "string") ? req.query.item : "";
    if (!busted && tag.length > 0) {
        const product = rewardManager.rewardQuantityProduct(tag, parseInt(req.query.qty) || 1);
        if (product.success && product.data.isInventoryItem) {
            const item = product.data.item;
            await inventoryManager.addInventoryItems(personaId, [{
                EntitlementTag: [item.entitlementTag],
                Hash: [`${item.hash}`],
                ResellPrice: [`${item.resalePrice || 0}`],
                RemainingUseCount: [`${product.data.quantity}`],
                VirtualItemType: [item.productType]
            }]);
            result.item = `${item.entitlementTag} x${product.data.quantity}`;
        }
    }

    log.game(`[PursuitProbe] pursuit ${busted ? "BUSTED" : "EVADED"}: +${result.cash} cash, +${result.rep} REP, heat ${heat}${result.item ? ", lucky draw " + result.item : ""}${result.leveledUp ? ", LEVEL UP " + result.level : ""}`);
    res.type("application/xml").send(`<PursuitProbeReward><Cash>${result.cash}</Cash><Rep>${result.rep}</Rep><Level>${result.level}</Level><LeveledUp>${result.leveledUp ? "true" : "false"}</LeveledUp><Item>${result.item}</Item></PursuitProbeReward>`);
});

module.exports = app;
