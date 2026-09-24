# NFSWorldPursuitProbe

ASI diagnostico per il client **2015** di NFS World.

> **Dipendenze**: zero setup esterno richiesto. Il probe di base (lettura
> di `D1FA88`/`D1FA90`) non usa librerie esterne; l'esperimento "innesca
> su qualsiasi urto" (`CollisionTrigger.cpp`) usa **Detours** (Microsoft,
> licenza MIT) per agganciare `CameraLogic_Update`, ma i sorgenti sono
> già inclusi in `third_party\detours\` e vengono compilati direttamente
> dentro il DLL — non serve una cartella `External\` o un `detours.lib`
> precompilato. Apri `.sln` e compila, basta.

## Perché esiste

Nella beta 2010, colpire un'auto della polizia in freeroam faceva
transitare il gioco allo stato `PURSUIT_SP`, tramite
`GameCore.Cops.LaunchPursuit` (esposta allo scripting via EASharp).
Nel client 2015 quella stringa (`PURSUIT_SP`) e l'intera API EASharp
`GameCore.Cops.*` sono **completamente assenti** dal binario — non è
nascosto, è stato rimosso insieme al vecchio sistema di pursuit
single-player, sostituito da eventi pursuit multiplayer.

Analizzando lo stesso binario 2015 però emerge un puntatore globale
ancora vivo e usato ovunque nel codice:

- `ds:0xD1FA88` (offset di modulo `0x91FA88`) — probabile puntatore a
  un oggetto "pursuit corrente"
- `ds:0xD1FA90` (offset di modulo `0x91FA90`) — probabile
  contatore/flag associato

Letti in 76 punti diversi del codice, incluso il blocco "sei
circondato dalla polizia" della telecamera (`byte_CC5A70`). Nessuna
scrittura diretta trovata via analisi statica del `.text` — quindi non
sappiamo ancora né dove viene costruito, né quale slot del vtable
avvia un pursuit (l'equivalente del vecchio `vtable+0x54` del 2010).

Questo ASI non tocca alcuna funzione del gioco (nessun hook/detour):
apre solo un thread che **legge periodicamente** questi due indirizzi
e logga ogni cambiamento, così possiamo correlare i log con azioni
reali in game e trovare il punto giusto da agganciare per la Fase 2
(il vero ripristino del trigger).

## Come si usa

1. Compila in **Release|Win32** (produce `NFSWorldPursuitProbe.asi`).
2. Copia `NFSWorldPursuitProbe.asi` nella cartella del client 2015,
   assieme a [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
   (necessario un `dinput8.dll`/`version.dll` loader, se non lo hai già
   per altri mod).
3. Avvia il gioco. Si apre una console "NFSWorldPursuitProbe" e viene
   scritto anche `NFSWorldPursuitProbe.log` nella cartella del gioco.
4. Gioca in freeroam. **Premi F12 nell'istante esatto** in cui succede
   qualcosa di rilevante per il pursuit:
   - colpisci un'auto della polizia
   - entri/esci da un evento pursuit esistente (se ancora presente nel
     multiplayer)
   - vieni "busted" o eludi la polizia in un evento
   - qualunque altra cosa cop-correlata

   Ogni pressione stampa una riga `[MARK]` nel log, da confrontare
   con le righe `CHANGE` circostanti.
5. **F11** forza in qualsiasi momento un dump extra: vtable di
   `objectPtr` (se leggibile) + una finestra di 21 dword grezze attorno
   sia a `objectPtr` che a `countAddr`. Serve soprattutto se `objectPtr`
   risulta **costante per tutta la sessione** (visto nella prima run:
   `0x00BCFA94` fisso dall'avvio) — in quel caso non è un oggetto
   allocato a runtime ma quasi certamente un singleton statico già
   vivo dal boot del gioco, quindi il campo utile da guardare è
   `count/flag`, non il vtable. Premi F11 quando vuoi ispezionare i
   dati grezzi attorno a entrambi gli indirizzi in un dato istante
   (es. subito dopo un F12).
6. Mandami (o incollami) `NFSWorldPursuitProbe.log` dopo la sessione.

### Nota sulla prima run

Nel primo log ricevuto, `objectPtr` è passato una sola volta da
`0x00000000` a `0x00BCFA94` pochi decimi di secondo dopo l'avvio, e da
lì è rimasto fisso per tutta la sessione — mentre `count/flag` è
cambiato più volte (0→1→2→1) senza che fosse premuto F12. Questo
significa:
- il tentativo di dump del vtable è fallito solo per timing (troppo
  presto dopo l'avvio, memoria non ancora pronta) — ora c'è un retry
  automatico per ~2s, oltre a F11 per riprovare manualmente quando
  vuoi.
- il segnale che varia durante il gioco è **`count/flag`**, quindi le
  prossime run vanno concentrate su quello: premi F12 ogni volta che
  fai qualcosa di rilevante, così le righe `CHANGE ... count/flag: X -> Y`
  vicine a un `[MARK]` diventano leggibili.

## Cosa guardare nel log

```
CHANGE objectPtr: 0x00000000 -> 0x0A1B2C3D   count/flag: 0 -> 1
  vtable = 0x00BC1234 (module-relative 0x7C1234)
    [ 0] +0x00 -> 0x00A10000 (module-relative 0x610000)
    ...
```

Se una riga `CHANGE` compare pochi istanti dopo (o prima) di un
`[MARK]`, quello è il segnale che stavamo cercando. L'indirizzo del
vtable e i suoi primi slot servono per la Fase 2 (capire quale
funzione chiamare per avviare il pursuit).

## Esperimento: forzare il trigger su qualsiasi urto

Il codice vanilla che avvia un pursuit negli eventi SP **non** è una
funzione singola riciclabile come `LaunchPursuit` nel 2010: nel 2015 è
un sistema a fabbrica basato su nomi (registry a `ds:0xC8178C`), dove
classi come `AICopMgrSpawn` e `AIPursuit` vengono registrate con
nome+dimensione oggetto, e l'evento SP le istanzia leggendo la sua
configurazione. Non c'è un singolo indirizzo fisso da chiamare.

Quello che invece POSSIAMO fare in modo relativamente sicuro: la
struttura che stiamo già osservando a `ds:0xD1FA88` ha, tra i suoi
campi, un contatore (`ds:0xD1FA90`, quello che nei log chiamiamo
"count/flag") che il gioco stesso porta a valori >0 durante il gioco
normale. `CollisionTrigger.cpp` aggancia `CameraLogic_Update` (stesso
hook già verificato in `NFSWorldCrap`) e, quando rileva un impatto con
**qualsiasi veicolo** (stessa euristica classId + vettore d'impatto già
usata per lo swing della telecamera — qui più adatta, perché per "ho
urtato qualcosa" un segnale di jolt è più naturale che per un
near-miss), forza il contatore a `1`.

**Nota sui falsi positivi**: il segnale classId+vettore d'impatto è lo
stesso già usato (e già noto per essere impreciso) nell'hook della
telecamera — reagisce a sterzate brusche e dossi, non solo a urti veri.
Una prima sessione ha mostrato il detector scattare anche solo guidando
normalmente. `CollisionTrigger.cpp` parte quindi con una soglia più
alta (`x3.0` invece di `x0.8`) e due tasti per regolarla **a runtime**
senza ricompilare:
- **PageUp**: meno sensibile (soglia più alta)
- **PageDown**: più sensibile (soglia più bassa)

Ogni volta che scatta, il log mostra `mag=` e `threshold(x...)=`: se
vedi scattare senza aver urtato nulla, premi PageUp finché smette di
succedere durante la guida normale, poi verifica che scatti ancora su
un urto vero.

**Per restare prudenti**, lo fa SOLO se lo slot dati adiacente
(`+0xC` dalla struttura, quello che nei log abbiamo visto valere
`0x2200C2C4`) contiene già un valore non nullo — cioè riusa quello che
il gioco stesso ci ha messo, non inventa mai un puntatore. Se lo slot è
vuoto (`0`), salta l'esperimento per quell'urto invece di rischiare che
qualche altro pezzo di codice del motore, vedendo `count>0`, provi a
leggere un oggetto che non esiste.

Guarda `NFSWorldPursuitProbe.log` per le righe `[collision-trigger]`
dopo ogni urto, e soprattutto **osserva il gioco stesso**: se qualcosa
di visibile succede (audio, HUD, comportamento IA), è il segnale che
stavamo cercando. Se non succede nulla — cosa più probabile, dato che
non sappiamo cosa rappresenti davvero quello slot — passiamo
all'opzione più invasiva (istanziare `AICopMgrSpawn`/`AIPursuit`
direttamente dalla fabbrica).

## Prossimo esperimento: trovare il campo che distingue un'auto della polizia

Il problema attuale: `classId` letto in `CollisionTrigger.cpp` è **il tuo**,
non quello di cosa hai colpito (confermato leggendo il disassembly di
`sub_7DA0F0`, la funzione che calcola il vettore d'impatto -- usa il tuo
classId solo come indice in una tabella di "durezza molla" per il
calcolo, non riceve né espone alcun riferimento all'oggetto colpito).
Ho anche verificato che le stringhe `numberOfPlayerCarCollisions` e
`MCivi2CopCollision` esistono nell'eseguibile ma portano solo a codice
di reportistica statistiche di fine evento (conta totali a fine gara),
non a un evento per-collisione utilizzabile in tempo reale.

Continuare a leggere disassembly alla cieca per trovare "il campo
giusto" non converge in tempi ragionevoli. Quindi invece ho aggiunto un
dump automatico: ogni volta che scatta `[collision-trigger] impact
detected`, il log ora stampa anche un blocco:

```
[collision-trigger] ---- HIT #N context dump ----
  transform +0x00 = 0x.......  (f=...)
  ...(fino a +0xFC)...
  cameraState +0x00 = 0x.......  (f=...)
  ...(fino a +0x11C, copre i flag +0xb1/+0xb6/+0xc8/+0xf5 già noti dal blocco "circondato dalla polizia")...
[collision-trigger] ---- end HIT #N ----
```

**Cosa fare**: fai due sessioni separate (o la stessa sessione, basta
segnare bene quale hit è quale nel messaggio che mi mandi):

1. Urta un'auto normale (traffico) o un muro, lontano da qualsiasi
   auto della polizia.
2. Urta un'auto della polizia (quella che hai già ripristinato in
   freeroam), possibilmente più volte per avere più campioni.

Poi mandami il log (o solo i blocchi `---- HIT #N ----` interessati).
Confronterò dword per dword i due dump: se esiste un campo (classId
dell'altro veicolo, un flag, un puntatore all'attore colpito, un
contatore che si comporta diversamente) che cambia in modo consistente
solo quando colpisci la polizia, quello diventa il segnale per
filtrare `TryForcePursuitFlag()` così che scatti solo su un vero urto
con un'auto della polizia.

**Risultato del confronto** (HIT#1=muro, HIT#2=polizia, HIT#3/4=veicolo
casuale): nessun campo nella finestra `transform`/`cameraState` isola
la polizia in modo pulito -- tutte le differenze o sono valori fisici
continui (cambiano comunque ad ogni urto, indipendentemente da cosa hai
colpito) oppure è il MURO a differire dagli altri tre (fisica statica
vs dinamica), mai la polizia da sola. Confermato: quella finestra di
memoria non contiene l'identità di cosa hai colpito.

## Opzione 2 (in corso): istanziare/chiamare AIPursuit direttamente

Analizzando `nfsw_2015.exe` ho trovato una classe con **sette vtable**
(tipico C++ a ereditarietà multipla) il cui costruttore (offset modulo
`0x90980`, raggiungibile solo tramite la stessa fabbrica a nomi usata
ovunque nel binario, `ds:0xD1EF10` -- non chiamabile a indirizzo fisso)
scrive questi puntatori nell'oggetto:

```
[this+0x00] = vtable 0xBDD904
[this+0x04] = vtable 0xBDD900
[this+0x24] = vtable 0xBDD8EC
[this+0x30] = vtable 0xBDD8D0
[this+0x40] = vtable 0xBDD8CC
[this+0x4C] = vtable 0xBDD880   <-- questa è quella interessante
[this+0x54] = vtable 0xBDD86C
```

La vtable a `0xBDD880` (assegnata al sotto-oggetto a `+0x4C`) ha, allo
slot 17 (`+0x44`, cioè l'indirizzo `0xBDD8C4`), un metodo (offset
modulo `0x484D90`) che: legge un globale a `ds:0xD2037C`, calcola
l'hash della stringa `"AIPursuit"` (stessa funzione hash `0x752400`
usata da ogni registrazione nella fabbrica), la cerca nel registry
`ds:0xD1EF10` e, se trovata, la aggiunge come "goal" a un oggetto
raggiunto tramite `this - 0x4C` (cioè torna alla base di **questo
stesso oggetto** -- coerente col fatto che il `this` dentro quel
metodo è il puntatore al sotto-oggetto `+0x4C`). È la cosa più vicina
trovata finora a "il codice che avvia un pursuit" nel client 2015,
senza che sia un evento SP scriptato a pilotarlo.

**Non sappiamo ancora** se esiste già un'istanza viva di questa classe
in memoria, né se è un singleton. Prima di rischiare una `call` a
caso su un puntatore indovinato (crash quasi garantito), ho aggiunto
alla probe una scansione **di sola lettura**: premi **F10** in game e
l'ASI cerca in tutta la memoria del processo un puntatore che combaci
con una delle sette vtable qui sopra. Non scrive né chiama nulla --
solo riporta se (e dove) esiste un oggetto vivo con quella forma, più
un dump dei primi 12 campi per un controllo di coerenza.

**Prossimo passo**: premi F10 un paio di volte durante una sessione
normale (magari anche subito dopo un urto con la polizia, o mentre sei
in un evento pursuit multiplayer se ancora presente) e mandami il log.
Se trovo un hit vero, il passo successivo sarà leggere con calma i
campi dell'oggetto trovato per capire se è sicuro chiamare quel
metodo (`+0x4C` poi slot 17) e con quali precondizioni. Se non trovo
nulla, vuol dire che l'oggetto non esiste finché qualcosa non lo crea
esplicitamente (serve trovare quel trigger), oppure che le vtable
identificate sono sbagliate.

Se anche questa pista si esaurisce, il fallback resta l'euristica di
prossimità (`AITrafficManager`, anch'esso registrato nella stessa
fabbrica a nomi, non ancora esplorato in dettaglio): al momento
dell'urto, scansionare i veicoli vicini e vedere se il più vicino ha
`classId==0` (`AIVehicleCopCar`).

## Struttura

```
NFSWorldPursuitProbe/
  NFSWorldPursuitProbe.sln
  NFSWorldPursuitProbe.vcxproj
  NFSWorldPursuitProbe.vcxproj.filters
  src/
    main.cpp              - DllMain, avvia/ferma probe + collision trigger
    Probe.h/.cpp           - logica di polling, offset, dump grezzo a 2 livelli
    CollisionTrigger.h/.cpp - hook su qualsiasi urto (usa Detours)
    VtableScan.h/.cpp      - F10: scan di sola lettura per le vtable candidate (Opzione 2)
    Logger.h               - logger minimale (console + file), zero dipendenze
  third_party/detours/     - sorgenti Detours (Microsoft, MIT) vendorizzati:
                              detours.h/.cpp, modules.cpp, disasm.cpp, image.cpp
                              (creatwth.cpp e i disassemblatori offline
                              per altre architetture non sono inclusi,
                              non servono per l'hooking in-process su x86)
```

## Nota

Gli offset (`0x91FA88` / `0x91FA90`) sono relativi all'image base
standard `0x400000` del client 2015 analizzato in questa sessione. Se
la tua build ha un `nfsw_2015.exe` diverso (versione/patch diversa),
vanno riverificati.
