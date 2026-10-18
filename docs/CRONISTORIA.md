# Cronistoria tecnica

Testo spostato dal README. La versione da installare e compilare è solo **01.38** (`PSBox-PS4-01.38.pkg`, User-Agent `xCloud-PS4/1.0.38`). Le sigle 01.01–01.12 più sotto sono il diario, non istruzioni di installazione. Problemi aperti attuali: [`PROBLEMI.md`](PROBLEMI.md). Upstream: [PSBox PS5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5) di RafaelNGP, v0.9.8 / `f8b2726` — vedi [`NOTICE.md`](../NOTICE.md).

Stream 01.38: SDP con transport-cc / ext id 3 (come PS5); TWCC cap ≤ 40; REMB; mtu 1200; abandon senza `pc->close`; 1080HQ; Videodec2; present BGRA720 con blit SSE; 4 buffer VideoOut; diagnostica `last_stage.txt` / `trace_stream.csv`. Auto-update da `/data/psbox/update` via BGFT. Chiusura con `_exit(0)` e log fsync.

## Installazione da PC, senza il pad

Il file da inviare è `dist/psbox-install.bin`: shellcode raw, non un ELF. Il primo byte è il codice di `_start`, non `7F 45 4C 46`. `dist/psbox-install.elf` è lo stesso sorgente linkato, solo per leggerlo con readelf: il BinLoader, vedendo il magic ELF, lo carica come eseguibile e l’ELF precedente (crt e PLT verso libkernel) moriva prima di qualsiasi notifica. Il `.bin` di allora era la copia di quell’ELF, stessa dimensione.

Ordine, con GoldHEN già avviato e il BinLoader in ascolto:

1. Copia `PSBox-PS4-01.38.pkg` in `/data/pkg` via FTP sulla porta 2121 (login anonimo). Se in quella cartella ci sono più `PSBox-PS4-*.pkg`, il payload prende il nome di versione più alto. Il pacchetto di questa tree è `PSBox-PS4-01.38.pkg`. Da Windows, dalla root del repo:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ps4_deploy.ps1 -Pkg .\dist\PSBox-PS4-01.38.pkg
```

Lo script cancella i vecchi `PSBox-PS4-*.pkg` in `/data/pkg` (non tocca `DiPlay_*.pkg`), carica il nuovo e verifica con `--list-only`. Per i log dopo un test:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ps4_log.ps1
```

### Deploy senza aprire PowerShell (My Machines)

Un agente Cloud non vede la LAN. Sul PC Windows registra **una volta** il worker `adam-windows-ps4` in Avvio automatico (doppio click, niente comandi da digitare):

```text
tools\register-ps4-worker.cmd
```

Dopo il login Windows il worker resta online da solo. Per i deploy successivi: apri l’agente su [cursor.com/agents](https://cursor.com/agents) → **My Machines** → `adam-windows-ps4` e chiedi di mandare il pkg: l’agente esegue `tools\ps4_deploy.ps1` senza farti aprire PowerShell.

2. Dal PC Windows, PowerShell, nella cartella del file (oppure usa `scripts\send_payload.ps1`):

```powershell
$ip = "192.168.1.19"; $port = 9090
$bytes = [System.IO.File]::ReadAllBytes(".\psbox-install.bin")
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($ip, $port)
$stream = $client.GetStream()
$stream.Write($bytes, 0, $bytes.Length)
$stream.Flush()
$client.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
Start-Sleep -Seconds 2
$client.Close()
```

Lo stesso invio, se c’è `ncat` o `nc`: `ncat --send-only 192.168.1.19 9090 < psbox-install.bin`. Il loader legge i byte fino alla chiusura della scrittura, senza un prefisso di lunghezza. La prima notifica è «PSBox payload avviato». Il log è `/data/psbox/payload.log`, ogni riga con `fsync`. Il payload gira nel processo di GoldHEN, già fuori dalla sandbox.

Il payload, in quest’ordine, scrive ogni passo in `/data/psbox/payload.log` con `write` e `fsync`:

1. Se `XCLD00001` risulta installato, `sceAppInstUtilAppUnInstall` e attesa che `sceAppInstUtilAppExists` torni falso.
2. Installa il pkg scelto con `sceAppInstUtilAppInstallPkg` e attende il titolo presente.
3. Scrive `Fortnite` in `/data/psbox/autostart.txt`.
4. Avvia `XCLD00001` con `sceLncUtilLaunchApp` (utente iniziale da `sceUserServiceGetInitialUser`, flag che salta il controllo dell’aggiornamento di sistema).

I moduli `libSceAppInstUtil.sprx` e `libSceLncUtil.sprx` si aprono con `sceKernelLoadStartModule` e i simboli con `sceKernelDlsym`. Stesse chiamate di Itemzflow (LightningMods); i flag di lancio in OpenOrbis sono attribuiti a LightningMods. GoldHEN (SiSTR0) è il BinLoader e la cartella `/data/pkg`. La notifica a schermo usa `OrbisNotificationRequest` (OSM, header OpenOrbis).

All’avvio, dopo il login e la libreria, l’app legge `autostart.txt`, lo cancella e avvia il gioco il cui nome o title id coincide (anche solo contenuto nel nome). `Fortnite` trova il riquadro Fortnite. Il log dell’app dice `autostart letto e cancellato` e `autostart: avvio`.

## Cosa provare

1. Avvio: deve comparire la schermata di login, non un crash immediato.
2. Login con codice dispositivo: il codice e il QR vanno inseriti su `microsoft.com/link` da telefono o PC. Dopo l’accesso la lista Game Pass si carica.
3. Apri un gioco. Dopo «video: decodifica solo con Videodec2» il log `/data/psbox/xcloud.log` deve continuare con `webrtc: logger`, `webrtc: config`, `webrtc: ctor`, qualche `mtx rec` / `mtx rc 0`, poi `pool`, quattro `worker`, `pool ok`, poi `sctp`, `usrsctp`, `rwlock 0`, `rwinit 1 rc 0`, `rwinit 2 rc 0`, `mtx`, `mtx rc 0`, `cond`, `cond rc 0`, `usrsctp ok`, `dtls`, `srtp`, `ice`, `ice ok`, `pc body`, `cert`, `cert gen`, `entropy`, `drbg 0`. Per ogni thread interno compaiono `pthread …`, `go …`, `rc 0` e, dal thread, `in … fs …`. Se usrsctp chiede un socket grezzo compare `socket no …`. Poi `webrtc: ctor ok`. Le righe `rtc:` compaiono solo per avvisi ed errori. Non c’è il QR verso GitHub né il prompt che installa `PPSA99810.zip`. All’avvio, se la sessione precedente è morta, compare `the last session went down: last line (…)` con l’ultima riga finita su disco.
4. Pad DS4, per posizione: Croce=A, Cerchio=B, Quadrato=X, Triangolo=Y, L1=LB, R1=RB, L2=LT, R2=RT, L3/R3=clic stick, Share=View, Options=Menu, PS=Guide. Il clic del touchpad apre il menu dell’app e non viene inviato come View. In quel menu, e anche in Impostazioni, c’è **Invia log**: manda il log della sessione precedente (`/data/psbox/xcloud.prev.log`) a pezzi da al massimo 3 KB, prima la fine del file. Ogni POST inizia con `parte X di N`. L’avviso è «Log inviato» oppure «Invio fallito». Non parte da solo. Lo User-Agent delle richieste è `xCloud-PS4/1.0.38`. Dopo uno stream guarda anche `/data/psbox/last_stage.txt` e `trace_stream.csv` (present_us / over budget).
5. In stream: `udp: RCVBUF 0x1002 = …` una sola volta; ogni secondo `rtcp: RR+REMB … twcc N` e `stream: … rtp N pkts … rtcp N twcc M` (rtp > 0). Chiudi dal menu PS: `exit: status …` / `exit: chiusura` / `exit: _exit`, senza CE-34878-0.
6. Se il login fallisce, leggi `/data/psbox/xcloud.log`.

## Compilare

Serve clang 18, `lld-18`, `llvm-18`, CMake, Ninja, Python 3 e `libssl.so.1.1` (PkgTool è un runtime .NET: Ubuntu 24 ha solo OpenSSL 3).

```bash
./scripts/build_pkg.sh
```

Lo script scarica OpenOrbis v0.5.4, mbedTLS 3.6.2, libdatachannel 0.23.1 e opus 1.5.2. Il risultato è `dist/PSBox-PS4-01.38.pkg` e `dist/psbox-install.elf`. Solo il payload: `./scripts/build_payload.sh`. La chiave del webhook, se serve, sta in `src/platform/ps4/secrets.h` (non è nel repository). Senza quel file l’invio del log resta spento.

## Cosa è stato importato

**Così com’è, da PSBox.** Auth Microsoft, Xbox Live, GSSV, catalogo, UI disegnata su canvas, sessione WebRTC (`src/stream/*`, `src/xcloud/gssv.cpp`), `cmake/deps/libdatachannel.cmake`, `cmake/deps/mbedtls.cmake`, `cmake/modules/FindMbedTLS.cmake`, `src/platform/ps5/net_probe.cpp`. Il link statico è lo stesso di `tools/ps5/link.sh`: libdatachannel, libjuice, libsrtp2, usrsctp, mbedTLS.

**Adattato.** `src/platform/ps4/libc_compat.c` (stessi shim della PS5: `getaddrinfo` via `sceNetResolver`, `FIONBIO`/`O_NONBLOCK` tradotti in `SO_NBIO`, `syscall` per il thread id, `timingsafe_bcmp`). I thread che non chiedono uno stack (il pool di libdatachannel, usrsctp) ne ricevono uno da 2 MiB; video, audio e worker restano a 8 MiB. Ogni thread passa da un trampolino che scrive `in` prima della routine vera. `pthread_set_name_np` è un no-op (la libc di Orbis non lo esporta) e `--wrap=socket` rifiuta `AF_ROUTE` e `SOCK_RAW`. I `pthread_rwlock` di usrsctp sono un mutex: il rwlock di Sony, diverso da quello di musl, faceva morire la 01.02 dentro la seconda `pthread_rwlock_init`. La 01.03 arrivava a `rwinit 2 rc 0` e moriva nella `pthread_mutex_init` successiva: l’attributo di Sony è un puntatore da 8 byte, quello di musl ne ha 4. La 01.04 passava comunque un attributo ricorsivo da 8 byte e, su Fortnite, moriva dopo sei `mtx 2 rc 0`, prima di `pool`. La 01.05 inizializza ogni mutex con `NULL` (come i mutex che già reggono login e log). Quelli ricorsivi restano rientranti con un contatore nello shim, senza un secondo oggetto Sony. Ogni riga del log fa `write` e `fsync`. Il rename di `xcloud.log` in `xcloud.prev.log` avviene prima di aprire il file nuovo. `src/net/http.cpp` imposta `sin_len`. `src/app/stream_player.cpp` manda l’Opus dello stream a `audio_orbis` e, senza FFmpeg, presenta il frame hardware. `src/app/ps5_main.cpp` usa il clic del touchpad come menu. `cmake/deps/mbedtls_user_config.h` disattiva l’entropia di piattaforma. Tre `std::min` in `src/ui/app_ui.cpp` per la libc++ di OpenOrbis.

**Nuovo, solo piattaforma.** `src/platform/ps4/` (rete, entropia, header FreeBSD mancanti per usrsctp), `src/display/display_ps4.cpp`, `src/input/controller_ps4.cpp`, `src/media/hw_decoder_ps4.cpp`, `src/media/media_stub_ps4.cpp`. Niente Vulkan, FSR, grilletti adattivi, updater PS5.

**Audio.** `third_party/moonlight-ps4/src/audio/audio_orbis.c` è il renderer di [Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4), attribuzione in `third_party/moonlight-ps4/PROVENIENZA.txt`. Unica modifica: il buffer di decodifica è da 960 campioni (frame Opus da 20 ms). Opus è libopus 1.5.2 (BSD).

## Diario 01.01–01.12

Login, lista, copertine e UI sono stati provati su GoldHEN 11.52. Lo stream usa Videodec2 (loader di Moonlight-ps4): se il decoder non parte, l’app torna alla schermata di errore con il codice, senza passare dallo stub FFmpeg. La 01.01 moriva dopo quattro `pthread 8388608<-0`. La 01.02 superava il pool e moriva dentro la seconda `pthread_rwlock_init` di usrsctp (`rwinit 2`). La 01.03, su Fortnite e firmware 11.52, superava Videodec2 (`libSceVideodec2` caricato, `CreateDecoder 0`) e moriva dopo `rwinit 2 rc 0`. Il file via FTP era da 7585 byte e finiva su quella riga intera; la copia arrivata al webhook era più corta (si fermava su `libSceVdecCo`) e non è il file su disco. La 01.04, sullo stesso gioco, moriva dopo sei `mtx 2 rc 0` (i mutex ricorsivi del PeerConnection) e non arrivava a `pool`. `Invia log` ha funzionato, ma il webhook taglia il corpo intorno a 3,9 KB: la 01.05 manda più POST da meno di 3 KB (`parte X di N`), prima l’ultimo pezzo. Lo User-Agent di quella serie era `xCloud-PS4/1.0.37`. La 01.06, dopo il login, legge `/data/psbox/autostart.txt` (una riga, il nome del gioco), cancella il file e avvia quel titolo. Su Fortnite 11.52 la 01.06 arrivava a Provisioned, audio SYSTEM, Videodec2 1920x1088 e cinque `mtx rc 0` dentro il ctor della PeerConnection, poi niente: niente `pc body`, niente ICE. `pthread_mutex_t` di musl è 40 byte e l'init di Sony scrive un oggetto più grande, nel membro C++ dopo. La 01.07 tiene quel mutex in un buffer da 256 byte e scrive `cb`, `pc members`, `init enter`, `pool thread`, `socket`, `cond`, `rand` a ogni passo. Su Fortnite 11.52 la 01.07 supera i `cb` e arriva a `init hw`, poi muore. La 01.08 (versione `1.0.8-ps4`) si ferma sulla stessa riga, senza `init nproc`: nel binario, fra i due log c'è `PLOG_DEBUG` (singleton di plog e, se la severità è debug, un `Record` con iostream). `hardware_concurrency` è dopo `init nproc` e chiama `__wrap_sysconf`, quindi quello shim non è mai stato eseguito. La 01.09 non chiama `hardware_concurrency` e non esegue quel `PLOG_DEBUG`: il pool è fisso a 4. `sysconf` di musl, l'unico punto che chiamava `cpuset_getaffinity`, non è più nel binario. La 01.09 si ferma su `init enter` (fra quel log e `init hw` c’è solo il byte `mInitialized`, già azzerato dal costruttore; la riga è su disco prima di `noteProgress`). La 01.10 scrive `init flag` e, su SIGSEGV/SIGBUS/SIGILL, RIP, indirizzo del fault e gli offset dalla base del modulo. Fortnite spariva perché il catalogo manda `XCloudTitleId` vuoto (la tessera veniva scartata) e, con Game Pass, `viaSubscription` lo toglieva da «I tuoi giochi»: l’id `FORTNITE` torna dalla lista dell’account. La 01.10 non stampa `init flag`: nel binario la lettura del flag è l’istruzione subito dopo `init enter`, e Fortnite non è fra i 21 di «I tuoi giochi». La 01.11 scrive `init back` prima di toccare il flag, prova un throw/catch all’avvio, e mostra i gratuiti in libreria e nella ricerca. La 01.12 mette Fortnite in cima, nella riga Preferiti (R3 aggiunge o toglie un gioco, la lista resta in settings.json), traduce il timeout di pthread_cond_timedwait nel valore 110 che libc++ si aspetta, e installa da sola un PSBox-PS4-*.pkg lasciato in /data/psbox/update/ all'avvio o da Impostazioni > Controlla aggiornamenti. Restano gli stub della 01.08 per le altre chiamate che su Orbis non ci sono: `getifaddrs` (netlink) con l'IP di `sceNetCtl`, `getentropy`/`getrandom`/`random`, `getauxval`, `prctl`, `sched_*affinity`, `if_nametoindex`, `/proc` e `/dev/urandom`, `CLOCK_BOOTTIME`, gli ioctl `SIOCGIF*` e `AF_NETLINK`. Il log aggiunge `init nproc`, `nproc`, `clock`, `ifaddr`, `ice gather`, `dtls ctor`, `dtls start` e `video: primo frame`. Il payload `psbox-install.elf` di quel giro non è stato eseguito su questa macchina: il controllo di allora verificava solo che il pacchetto avesse `APP_VER` 01.07. Il `thread_local` di libdatachannel è local-exec (`fs:0` meno 0xa20): le righe di log usano solo `fs:0-8`, quindi non dimostrano che quel blocco esista sul thread nuovo. Il present del frame è ancora la conversione CPU su sceVideoOut. Il tasto PS (spesso lo trattiene il sistema), i colori e la sessione di gioco vanno riprovati. L’entropia non è un RNG hardware. Il decoder software FFmpeg non è linkato.
