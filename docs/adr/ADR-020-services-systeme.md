# ADR-020 : Fournir les services du système par des portails C++ à backends interchangeables, sans exposer le système à la page

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-31 | Proposée | Ambroise Leclerc | Décision initiale : distinction hôte/client, préalables regroupés en épiques (#446 à #450), confinement des racines pendant la résolution, jetons de lecture réutilisables liés à la connexion par un cookie, exposition des jetons et transport des lectures distantes, exception de confiance explicite, liaison HTTP par profil de navigateur, racines déclarées accessibles à tout script admis, services en composants facultatifs, sélection par l'utilisateur valant autorisation (poignées plutôt que chemins), backends natif, HTML et navigateur choisis par connexion, contenu des fichiers par HTTP à jeton ; état de l'art et inventaire des autres services. | À renseigner après intégration. |

## État et chronologie

**Proposée** le **2026-10-31**. Aucun code n'est livré avec cette version : elle fixe le cadre des services
système et leurs préalables (épiques #446 à #450) avant l'implémentation du premier d'entre eux (sélection de fichiers). Elle passera à « Acceptée » quand les
critères de la section dédiée seront démontrés.

## Contexte

Une application WebFront a besoin de services que la page seule ne fournit pas, ou fournit mal : choisir un fichier
ou un dossier à ouvrir, choisir où enregistrer, parcourir des fichiers, copier dans le presse-papiers, ouvrir un lien
dans le navigateur par défaut, notifier l'utilisateur, connaître les répertoires standard de l'application…

### Deux machines, deux systèmes de fichiers

La particularité de WebFront, absente d'Electron, Tauri ou Wails, est que **la machine qui exécute le C++ (l'hôte)
n'est pas forcément celle qui affiche la page (le client)** :

| Configuration | Hôte = client ? | Fenêtre native disponible sur l'hôte ? | Exemple |
| --- | --- | --- | --- |
| Frontend intégré (CEF, WebView2, WKWebView, WebKitGTK) | oui | oui, et WebFront en connaît la fenêtre | application de bureau |
| Navigateur système, boucle locale | oui | oui, mais WebFront ne connaît pas la fenêtre du navigateur | `webfront::WebFront` par défaut |
| Linux embarqué en kiosque (Cog/WPE, Chromium `--kiosk`) | oui | souvent non : ni bureau, ni portail, ni gestionnaire de fenêtres complet | IHM d'équipement Yocto |
| Équipement sans écran, navigateur distant | non | non | microcontrôleur, passerelle, poste distant (ADR-017) |
| Plusieurs navigateurs connectés au même hôte | non pour au moins l'un d'eux | — | supervision partagée |

« Le système de fichiers » est donc ambigu. Les **fichiers de l'hôte** sont ceux que le C++ lit et écrit ; les
**fichiers du client** sont ceux que l'utilisateur possède sur la machine qui affiche la page. `<input type="file">`
ne donne accès qu'aux seconds, et sans leur chemin. Un dialogue natif de l'hôte ne montre que les premiers, et
seulement si l'hôte a un écran. Quand l'hôte et le client coïncident, les deux se rejoignent, mais par des voies
différentes.

### Ce qui existe aujourd'hui

- Le serveur HTTP ne traite que `GET` et `HEAD` ; les écritures passent par le pont, par morceaux séquentiels
  (modèle de l'installation `wf`, ADR-010).
- Les chaînes du pont sont bornées à 65 535 octets par leur en-tête (longueur sur 16 bits, `maxStringBytes`) ; ce
  n'est pas une limite de WebSocket. Les tableaux typés ont une longueur sur 32 bits et un message est borné à 64 Mio
  par défaut (`BuffersPolicy`). Le pont ne transporte ni liste de chaînes ni liste d'enregistrements, et un rejet ne
  porte qu'un message (ADR-003, ADR-004).
- `cppFunction` encode immédiatement le résultat du callback et `webfront::Task` (`ex::task<void>`) n'est pas un
  template : un callback ne peut pas différer sa réponse jusqu'à la fin d'une coroutine, d'un dialogue ou d'une lecture.
- Un fournisseur (ADR-002) ne voit que `open(path)` et rend un `fs::File` ; il ne voit ni la requête (`Range`), ni la
  connexion, ni des jetons émis à l'exécution. Le serveur copie le fichier entier dans la réponse avant l'envoi
  (`readFile`) : la mémoire croît avec la taille du fichier, ce qui interdit les gros fichiers sur cible embarquée.
- Une connexion `UI` n'est associée à aucune vue : rien ne distingue la page d'un frontend intégré d'un autre client.
- Aucun frontend n'expose son thread d'interface ni sa fenêtre native ; avec le navigateur système, aucune boucle de
  toolkit ne tourne sur le thread principal.
- Aucun adaptateur ne traite `<input type="file">` : ni `WKUIDelegate runOpenPanelWithParameters` pour WKWebView (sans
  lui, l'élément ne fait rien sous macOS), ni `CefDialogHandler` pour CEF, ni le signal `run-file-chooser` de WebKitGTK
  (son gestionnaire par défaut ouvre un dialogue GTK). WebView2 affiche le dialogue du système par lui-même. Ce
  comportement par moteur n'est pas couvert par la suite partagée.
- La politique d'origines (ADR-008) admet sur le pont toute page de l'origine applicative : un script admis partage
  l'autorité de l'application. Toute fonction de service enregistrée est donc appelable par ce script.
- L'automatisation (ADR-019) et les tests (ADR-012) supposent qu'aucune étape n'attend une action humaine dans une
  fenêtre que l'outil ne pilote pas : un dialogue natif modal est hors de leur portée.

### Contraintes de plateforme

- **Thread et parenté.** Les dialogues natifs s'ouvrent sur le thread de l'interface : STA COM pour `IFileOpenDialog`,
  thread principal pour `NSOpenPanel` et GTK. Sans fenêtre parente (navigateur système), le dialogue peut apparaître
  derrière le navigateur. Windows restreint le passage au premier plan ; Wayland demande un identifiant de fenêtre
  exporté (`xdg-foreign`) que WebFront n'a pas pour un navigateur externe.
- **Bacs à sable.** Flatpak/Snap (portail `org.freedesktop.portal.FileChooser`, fichiers exposés sous `/run/user/<uid>/doc/`),
  macOS en sandbox (accès accordé par `NSOpenPanel`, durable par signets « security-scoped »), Android (Storage Access
  Framework : URI `content://`, `takePersistableUriPermission`), iOS (`UIDocumentPickerViewController`, URL
  « security-scoped »), MSIX. Partout, **un chemin n'est pas une autorisation** : c'est le choix de l'utilisateur qui
  l'accorde, et sa persistance demande un objet propre au système.
- **Embarqué.** Pas de bureau, souvent pas de portail ni de bibliothèque de dialogue ; mémoire et stockage réduits ;
  système de fichiers racine en lecture seule, données sur une partition dédiée ; supports amovibles (USB, SD) montés
  à la demande ; sur microcontrôleur, FatFs ou LittleFS sans notion d'utilisateur ni de permission, et l'utilisateur
  toujours distant. Le budget des ressources embarquées du cœur (ADR-016, 150 Kio) interdit d'y ajouter un
  explorateur de fichiers.
- **Contexte sécurisé.** `http://127.0.0.1` et `http://localhost` sont des contextes sécurisés ; une adresse distante en
  HTTP clair ne l'est pas. L'API Clipboard asynchrone, les notifications Web, Web Serial et la File System Access API
  y sont donc indisponibles, ce qui touche précisément la configuration « navigateur distant ».
- **Disponibilité dans les moteurs.** `showOpenFilePicker`/`showDirectoryPicker` n'existent que dans Chromium (CEF,
  WebView2, Chrome, Edge), pas dans WebKit (WKWebView, WebKitGTK, Safari) ni dans Firefox. Une conception fondée sur
  eux ne serait pas portable entre frontends.

## État de l'art (2026-10-31)

### Bibliothèques web-natives de bureau

| Bibliothèque | Accès aux services | Modèle d'autorisation | Remarques pour WebFront |
| --- | --- | --- | --- |
| **Electron** | `dialog.showOpenDialog`, `shell.openExternal`/`showItemInFolder`, `clipboard`, `Notification`, `Tray`, `Menu`, `app.getPath`, `safeStorage`, `powerSaveBlocker` : processus principal seulement | Isolation de contexte, `contextBridge` d'un préchargement qui expose une API **choisie par l'application**, `ipcRenderer.invoke` | `File.path` retiré (Electron 32) au profit de `webUtils.getPathForFile` : même Electron cesse de donner des chemins à la page par défaut |
| **Tauri v2** | Greffons `dialog`, `fs`, `clipboard-manager`, `notification`, `opener`, `os`, `store`, `single-instance`, `deep-link`, `autostart`, `global-shortcut`… | **Capabilities** par fenêtre : permissions `greffon:commande` et portées (globs, refus prioritaire). Une fenêtre sans capability n'a aucun accès | Dialogue via `rfd` sur bureau, sélecteurs natifs sur Android/iOS ; `fs` gère les URI `content://` ; un greffon tiers persiste les portées accordées par un dialogue |
| **Wails** | Runtime Go : `OpenFileDialog`, `SaveFileDialog`, `MessageDialog`, presse-papiers, `BrowserOpenURL` ; v3 : menus, barre système, multifenêtre | Méthodes Go liées explicitement par l'application | Les services sont des appels du code natif, la page n'appelle que ce que l'application expose |
| **Neutralinojs** | `os.showOpenDialog`, `os.showFolderDialog`, `filesystem.*`, `clipboard`, `os.showNotification`, `os.setTray` | Liste `nativeAllowList`/`nativeBlockList` dans la configuration, jeton d'accès au serveur local | **Architecture la plus proche** (serveur local + WebSocket). Son mode « cloud » sert l'interface à un navigateur distant ; une API `filesystem.*` générique y expose alors l'hôte |
| **webview/webview** (C/C++) | `bind`/`eval`/`dispatch` seulement | — | Les applications ajoutent nativefiledialog, portable-file-dialogs ou tinyfiledialogs |
| **saucer** (C++23) | Module `saucer/desktop` : sélecteurs de fichiers, ouverture d'URI, position de la souris | Modules opt-in, fonctions exposées explicitement | Même public que WebFront ; confirme le découpage en module facultatif hors du cœur |
| **Qt WebEngine** | `QWebEnginePage::chooseFiles` surchargeable pour `<input type="file">` ; `QFileDialog`, `QDesktopServices`, `QStandardPaths`, `QClipboard`, `QSystemTrayIcon` | Code natif | `QFileDialog` choisit entre dialogue natif, portail (Flatpak/Snap, `QT_QPA_PLATFORMTHEME=xdgdesktopportal`) et **dialogue dessiné par Qt** (`DontUseNativeDialog`) : le principe du repli non natif est éprouvé |
| **CEF / WebView2 / WebKitGTK / WKWebView** | `CefDialogHandler::OnFileDialog`, gestion native de WebView2, signal `run-file-chooser`, `runOpenPanelWithParameters` | — | `<input type="file">` est un service que **l'adaptateur** doit rendre opérant, avant toute API nouvelle |

### Interfaces natives et multiplateformes hors web

| Bibliothèque | Approche |
| --- | --- |
| **Flutter** | Greffons fédérés (`file_selector`, `path_provider`, `url_launcher`, `share_plus`) : une API Dart, une implémentation par plateforme, dont une implémentation web |
| **.NET MAUI** | `FilePicker`, `Clipboard`, `Launcher`, `Share`, `SecureStorage`, `FileSystem.AppDataDirectory` ; enregistrement et choix de dossier dans le Community Toolkit |
| **Capacitor** | Greffons `Filesystem`, `Clipboard`, `Share`, `Preferences`, chacun avec une implémentation web de repli ; sélecteur de fichiers par greffon communautaire |
| **Slint, egui, Iced** (Rust) | Pas de dialogue intégré ; la crate `rfd` : COM sous Windows, `NSOpenPanel` sous macOS, **portail XDG par défaut sous Linux** (GTK3 en option) |
| **nativefiledialog-extended** (C/C++) | Même répartition que `rfd` (portail D-Bus ou GTK3 sous Linux), bibliothèque compilée |
| **portable-file-dialogs**, **tinyfiledialogs** | Sous Linux et macOS, lancent `zenity`, `kdialog` ou `osascript` ; des injections de commandes ont été corrigées dans tinyfiledialogs, et le résultat dépend des programmes installés |

### Embarqué

| Pratique | Approche |
| --- | --- |
| **LVGL** | Pilotes `lv_fs_drv` (FatFs, LittleFS, POSIX) et widget `lv_file_explorer` dessiné par la bibliothèque : l'explorateur fait partie de l'interface, pas du système |
| **Qt for MCUs** | Aucun dialogue de fichiers ; l'application dessine ses écrans |
| **IHM web d'équipements** (OpenWrt LuCI, Cockpit, OctoPrint, Home Assistant, exemple `file_server` d'ESP-IDF) | Le serveur liste ses fichiers dans des **racines déclarées**, la page les présente en HTML ; téléchargement par `GET`, envoi par `POST` multipart ; le fichier du client passe par `<input type="file">` |
| **Kiosques Linux** (Cog/WPE, Chromium `--kiosk`) | Pas de dialogue système : sélecteur dans l'interface, supports amovibles détectés par udev/udisks2 |

### Leçons retenues

1. **Le service est natif, l'API exposée est choisie par l'application.** Electron, Wails, saucer et Tauri ont tous
   abandonné ou évité l'accès générique de la page au système (`nodeIntegration`, `filesystem.*` sans portée).
2. **La sélection par l'utilisateur vaut autorisation** (portail XDG, sandbox macOS, SAF, iOS, Tauri) et se représente
   par une poignée ou une URI, pas par un chemin libre.
3. **Un même service a plusieurs implémentations** (natif, portail, dessiné par la bibliothèque, web) choisies selon
   la plateforme ; le repli dessiné par la bibliothèque (Qt, LVGL, IHM d'équipements) est la seule voie sans bureau.
4. **Sous Linux, le portail XDG est le chemin moderne** (Flatpak, Snap, Wayland, GNOME/KDE), GTK un repli.
5. **Lancer des programmes externes pour un dialogue** est fragile et a produit des failles.
6. **Le contrôle de `<input type="file">` revient au moteur intégré**, pas à une API nouvelle.

## Solutions comparées

| Solution | Bureau local | Navigateur système | Kiosque embarqué | Client distant | Mobile | Coût | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A. Dialogues natifs de l'hôte appelés depuis C++ | excellent, parenté connue | possible mais sans parent, focus incertain | souvent impossible | **faux** : s'ouvre sur l'écran de l'hôte | sélecteurs natifs à adapter | moyen, par plateforme | retenu comme backend |
| B. Sous-fenêtre HTML dans la page, alimentée par un listage C++ (« sous-fenêtre React ») | correct, aspect non natif | correct | **seule voie** | **seule voie pour l'hôte** | inadapté (SAF) | moyen, une fois | retenu comme backend, sans dépendance à React |
| C. Seconde fenêtre WebView dédiée au sélecteur | lourde | impossible | impossible sans gestionnaire de fenêtres | impossible | impossible | élevé | écarté |
| D. API du navigateur (`<input type="file">`, File System Access, Clipboard) | fichiers du client sans chemin | idem | idem | **fichiers du client** | idem | faible | retenu pour les fichiers du client |
| E. API générique de fichiers exposée à la page | — | — | — | expose l'hôte | — | faible | écarté |
| F. Bibliothèque tierce (nativefiledialog-extended, `zenity`) | correct | correct | non | faux | non | dépendance compilée ou processus | écarté du cœur, possible derrière le concept de backend |

Aucune solution ne couvre seule toutes les configurations : la réponse est un **service unique à backends
interchangeables**, dont le choix dépend de la connexion et non de l'application.

## Décision

### 1. Des services facultatifs, hors du cœur

Les services résident sous `include/webfront/services/<service>/`, sur le modèle des composants (ADR-010) : une façade
C++, des fonctions de pont nommées `webfront.services.<service>.<fonction>`, un module ES et ses déclarations
TypeScript. Ils ne sont actifs que si l'application les instancie, s'abonnent par `subscribeUIStarted` et n'ajoutent
rien aux ressources minimales du cœur (ADR-016). Les backends natifs s'activent par option CMake ; aucun ne devient une
dépendance du parcours par défaut (ADR-007).

### 2. La sélection vaut autorisation : des poignées, pas des chemins

Un service de fichiers ne donne jamais à la page un pouvoir sur un chemin arbitraire de l'hôte. Une autorisation
(`FileGrant`) naît de deux sources seulement, de portée de confiance différente :

- un **dialogue contrôlé par l'hôte** (dialogue natif, ou appel C++ de l'application) : la sélection y est réellement
  faite par l'utilisateur, car aucun script de la page ne peut piloter un dialogue du système ;
- une **racine déclarée accessible à la page** (sélecteur HTML) : la racine elle-même est l'autorisation. Le sélecteur
  HTML s'exécute dans la page, avec l'autorité du pont ; tout script admis (ADR-008) peut appeler les mêmes opérations
  de listage et de création de poignées sans interaction avec le dialogue. Le sélecteur est une interface, **pas une
  frontière d'autorisation**.

Dans les deux cas, l'autorisation se présente ainsi :

- côté C++, une autorisation porte le chemin réel ou l'objet de plateforme (URI SAF, URL « security-scoped », chemin
  du portail) et des droits (lecture, écriture, création dans un dossier) ;
- côté JavaScript, elle n'est qu'une **poignée opaque** liée à la connexion (`UI`) pour les opérations du pont (une
  autre connexion ne peut pas l'employer) ; ses lectures HTTP sont liées au profil du navigateur (règle 4) : identifiant aléatoire, nom
  affichable, taille, type MIME, droits ; jamais le chemin complet, sauf si l'application le demande explicitement ;
- une poignée meurt avec la connexion ou sur révocation ; une persistance entre sessions (fichiers récents) est un choix
  explicite de l'application, réalisé par les signets et URI persistantes de la plateforme.

Le code C++ de l'application, lui, reçoit l'autorisation complète : il fait partie de la base de confiance.

### 3. Le backend se choisit par connexion

Un service de fichiers déclare ses backends dans un ordre de préférence ; pour chaque appel, le premier **utilisable
pour cette connexion** est retenu :

1. **Natif** — utilisable seulement si la connexion est **associée explicitement à la vue d'un frontend intégré** de
   l'application (secret propre au lancement, présenté par la vue lors de la poignée de main) et si l'hôte a un
   affichage. Une adresse de boucle locale ne suffit jamais : un navigateur distant derrière un proxy local ou un
   tunnel, ou un autre utilisateur de la machine, y arrivent aussi. Avec le navigateur système, aucune association
   n'est possible : le natif y est indisponible **par défaut**. Seule exception, une option de confiance que
   l'application active explicitement dans sa configuration (par exemple `trustLoopbackClients = true`, désactivée par
   défaut) rend le natif utilisable pour les connexions de boucle locale ; elle réintroduit la déduction par adresse
   et n'est sûre que sur une machine mono-utilisateur sans tunnel ni proxy local. Windows : `IFileOpenDialog`/`IFileSaveDialog` ;
   macOS : `NSOpenPanel`/`NSSavePanel` par le runtime Objective-C déjà employé par WKWebView ; Linux : **portail XDG**
   `org.freedesktop.portal.FileChooser` par D-Bus, puis GTK quand WebKitGTK est déjà lié ; Android et iOS : sélecteurs du
   système, lors de leurs frontends. Le dialogue s'ouvre sur le thread de l'interface, avec la fenêtre de l'adaptateur
   pour parent quand elle existe.
2. **HTML** — un sélecteur dessiné dans la page : élément personnalisé dans un `<dialog>` modal, sans dépendance à
   React ni à une autre bibliothèque, alimenté par un listage paginé que le C++ calcule **uniquement dans des racines
   déclarées** par l'application (`roots` : données de l'application, supports amovibles, dossier d'export…). Sans
   racine déclarée, ce backend est indisponible. Chaque racine déclare son accès par la page : `none` (aucune opération
   depuis la page ; seuls le C++ et le dialogue natif créent des autorisations), `read` (lister et lire) ou `readWrite`.
   Une racine accessible est **entièrement** à la portée de tout script admis, dans ses droits : l'application ne doit
   y placer que ce qu'elle accepterait de confier à n'importe quel script de son origine. Une confirmation réellement
   contrôlée par l'hôte pour un client distant n'existe pas dans la page ; si l'application en a besoin, elle passe par
   un canal hors de la page (bouton physique de l'équipement, confirmation sur l'écran local), hors du périmètre de
   cette décision. Il est le seul utilisable en kiosque et pour un client distant, et
   le seul que les tests et l'automatisation pilotent (ADR-012, ADR-019). L'adaptateur React (`@webfront/react`) peut
   l'envelopper sans le posséder.
3. **Client** — pour les fichiers du client : `<input type="file">`, glisser-déposer et téléchargement par lien. Ce
   n'est pas un repli des deux premiers : l'API le distingue (`files.importFromClient()`, `files.exportToClient()`),
   car son résultat ne désigne aucun fichier de l'hôte.

L'application peut forcer un backend (`Backend::html` pour une IHM au rendu homogène, `Backend::native` sans repli
pour un outil de bureau) ; elle ne peut pas rendre le natif « utilisable » pour un client distant. Un dialogue annulé
renvoie une liste vide, pas une erreur ; l'absence de backend utilisable est une erreur (Promise rejetée, `get()` qui
lève).

### 4. Le contenu des fichiers ne passe pas par des chaînes du pont

- Lecture : une poignée produit une URL `GET` à jeton ; le contenu peut alimenter `<img>`, `<video>`, `fetch` ou un
  téléchargement (règles du jeton ci-dessous). Un fournisseur de l'ADR-002 ne suffit pas :
  il ne voit ni la requête ni les jetons. Cette lecture exige une **extension du traitement HTTP** : des routes
  dynamiques dont le gestionnaire reçoit la requête complète et produit statut, en-têtes et corps ; un envoi **en flux**
  par morceaux bornés, à la place de la copie du fichier entier dans la réponse, pour que la mémoire ne dépende pas de
  la taille du fichier, sur poste comme sur cible embarquée ; les plages `Range`/`206`/`Content-Range` traitées par le
  serveur sur un `fs::File` capable de se positionner, ou par le gestionnaire de la route.
- Écriture : tant que le serveur ne traite que `GET`/`HEAD`, par morceaux de tableaux typés sur le pont, écrits dans un
  fichier temporaire puis validés par renommage, comme l'installation `wf` (ADR-010). L'ajout de `PUT`/`POST` au serveur
  relèvera de sa propre décision.
- **Jeton de lecture.** Le jeton désigne une autorisation, pas une requête : il est **réutilisable** pendant la vie de
  l'autorisation et de la connexion, pour que les requêtes `Range` successives d'une vidéo (positionnement, reprise
  après pause ou coupure) aboutissent toutes. Il est aléatoire (au moins 128 bits), comparé en temps constant, absent
  des journaux, et révoqué à la fermeture de la connexion, à la révocation de l'autorisation ou à son expiration
  facultative. Avant l'expiration, la page obtient un nouveau jeton par la poignée (`handle.url()` rappelé) ; un élément
  média dont le jeton expire en cours de lecture reprend à la position courante avec la nouvelle URL. Un jeton à usage
  unique n'est qu'une option explicite pour un téléchargement ponctuel, jamais pour un média.
- **Liaison au navigateur, pas à la connexion.** Une requête HTTP ne porte pas l'identité de la WebSocket, et `<img>`
  ou `<video>` ne peuvent pas ajouter d'en-tête : un jeton seul est une capacité au porteur. Par défaut, une lecture
  exige donc le jeton **et** un cookie de liaison : `WebFront.js` échange, par une requête `fetch` de même origine, un
  secret à usage unique reçu par le pont contre un cookie `HttpOnly`, `SameSite=Strict`, limité au chemin des
  services. Un cookie est partagé par tous les onglets d'un même profil pour une origine : il identifie donc un
  **client** (profil de navigateur), pas une connexion. Le serveur associe à ce client l'ensemble de ses connexions ;
  une connexion qui présente déjà un cookie valide lors de l'échange rejoint ce client au lieu d'en créer un. Un jeton
  est accepté si le cookie présenté désigne le client de la connexion propriétaire, tant que celle-ci est ouverte. La
  garantie est la possession du cookie : la même URL rejouée depuis un **autre client** (autre navigateur, autre
  profil, autre machine, client HTTP) est refusée (`403`, sans indiquer si le jeton existe), mais une autre page de la
  même origine dans le même profil peut la lire. Ce n'est pas une perte : ces pages partagent déjà stockage, cookies et
  autorité du pont (ADR-008). Une isolation par connexion compatible avec les éléments média exigerait un service
  worker qui ajoute un en-tête selon la page émettrice, indisponible hors contexte sécurisé (client distant en HTTP)
  et aux plages média encore inégalement interceptées : elle est écartée. Si le navigateur refuse le cookie, la lecture échoue explicitement, sans repli. Un **mode transférable**,
  explicite et à durée courte obligatoire, fait du jeton seul une capacité au porteur, documentée comme telle, pour
  remettre une URL à une application externe.
- **Exposition du jeton et du contenu.** Chaque réponse de lecture porte `Cache-Control: no-store` et
  `Referrer-Policy: no-referrer` ; le jeton n'apparaît dans aucun journal HTTP ni diagnostic (chemin masqué). WebFront
  ne fournit pas TLS (ADR-008) : sur la boucle locale, le contenu ne quitte pas l'hôte ; pour un client distant, une
  lecture en HTTP clair expose jeton, cookie et contenu à tout observateur du réseau. Les lectures et écritures de
  fichiers pour un client distant exigent donc un transport chiffré fourni par le déploiement (mandataire inverse
  TLS, tunnel chiffré, réseau isolé de confiance) ; sans lui, l'application doit l'accepter explicitement
  (`allowPlaintextRemote`, désactivé par défaut), et le service refuse sinon la lecture distante. Le cookie de liaison
  porte `Secure` lorsque la page est servie en HTTPS.
- Listage : paginé, par un type filaire « liste » de chaînes et de tuples à ajouter à l'ADR-003 ; le codage d'une
  liste en JSON dans une chaîne est écarté.

### 5. Appels depuis C++ et depuis JavaScript

Esquisse ; les noms ne sont pas contractuels avant l'implémentation. `cppFunction` répond aujourd'hui dès le retour
du callback : la forme ci-dessous suppose la **réponse différée** des préalables (un callback qui renvoie une tâche à
résultat, `ex::task<R>`). La Promise JavaScript se résout quand la tâche se termine, avec la valeur encodée ; elle se
rejette si la tâche lève (message et code d'erreur typé) ou se termine arrêtée (code d'annulation), et à la
déconnexion, qui demande l'arrêt de la tâche et écarte sa réponse.

```cpp
webfront::services::Files files{app, {.roots = {{"Données", dataDirectory}}, .backends = Backends::automatic}};

app.cppFunction("importer", [&](App::UI ui) -> ex::task<int> {
    auto picked = co_await files.open(ui, {.title = "Importer", .filters = {{"Images", {"*.png", "*.jpg"}}}});
    for (auto& grant : picked) load(grant.path());   // C++ : autorisation complète
    co_return static_cast<int>(picked.size());       // résout la Promise de la page
});
```

Sans cette extension, une application peut seulement lancer le travail avec `ui.spawn(task)` et répondre aussitôt ;
la Promise se résout alors avant la fin du dialogue, et le résultat comme les erreurs doivent revenir par un appel
`ui.jsFunction` séparé. Cette forme dégradée ne sert pas de contrat aux services.

```ts
import { files } from "@webfront/services/files";
const [image] = await files.open({ filters: [{ name: "Images", patterns: ["*.png"] }] }); // poignée opaque, [] si annulé
if (image) img.src = image.url();
```

Chaque appel de dialogue est une tâche possédée (ADR-006), annulable localement ; la fermeture de la connexion ferme
le sélecteur HTML et abandonne le résultat d'un dialogue natif encore ouvert. La façade JavaScript n'est enregistrée
que pour les opérations que l'application autorise depuis la page (`allowFromPage`) ; par défaut, ouvrir et enregistrer
un dialogue natif sont permis car l'utilisateur confirme dans une interface que la page ne pilote pas ; lister une racine
et y créer des poignées dépendent seulement de l'accès déclaré pour cette racine, que l'appel vienne du sélecteur HTML ou
de tout autre script admis.

### 6. Préalable : rendre `<input type="file">` opérant dans chaque frontend intégré

Avant tout service nouveau, chaque adaptateur doit traiter le sélecteur standard : `runOpenPanelWithParameters` pour
WKWebView, `CefDialogHandler` ou l'implémentation par défaut qualifiée pour CEF, le signal `run-file-chooser` (portail
sous Wayland/Flatpak) pour WebKitGTK, la gestion de WebView2 vérifiée. Un scénario de la suite partagée l'atteste.
C'est le service minimal qu'attend tout développeur web, et il est gratuit pour les fichiers du client.

## Préalables

Le code vérifié le 2026-10-31 ne permet pas encore d'implémenter ces règles. Les préalables sont regroupés en cinq
épiques, dans l'ordre ; les étapes 1 et 2 sont indépendantes et peuvent avancer en parallèle.

| Étape | Épique | Tâches | Débloque |
| --- | --- | --- | --- |
| 1 | [#446](https://github.com/ambroise-leclerc/WebFront/issues/446) Comportements web standard dans les frontends intégrés | [#451](https://github.com/ambroise-leclerc/WebFront/issues/451) `<input type="file">` et téléchargements ; [#452](https://github.com/ambroise-leclerc/WebFront/issues/452) `alert`/`confirm`/`prompt` | fichiers du client (règle 6) ; corrige un défaut existant |
| 2 | [#447](https://github.com/ambroise-leclerc/WebFront/issues/447) Connexions et routes du serveur HTTP | [#453](https://github.com/ambroise-leclerc/WebFront/issues/453) association explicite d'une `UI` à la vue d'un frontend ; [#454](https://github.com/ambroise-leclerc/WebFront/issues/454) routes dynamiques et envoi en flux ; [#455](https://github.com/ambroise-leclerc/WebFront/issues/455) `Range` (206) ; [#469](https://github.com/ambroise-leclerc/WebFront/issues/469) liaison des requêtes HTTP à la connexion (jeton et cookie) | choix du backend (règle 3), lecture par jeton (règle 4) |
| 3 | [#448](https://github.com/ambroise-leclerc/WebFront/issues/448) Types et contrats du pont | [#456](https://github.com/ambroise-leclerc/WebFront/issues/456) chaînes longues (32 bits) ; [#457](https://github.com/ambroise-leclerc/WebFront/issues/457) listes de chaînes et de tuples ; [#458](https://github.com/ambroise-leclerc/WebFront/issues/458) erreurs typées ; [#459](https://github.com/ambroise-leclerc/WebFront/issues/459) réponse différée des callbacks C++ | listages (règle 4), appels (règle 5) |
| 4 | [#449](https://github.com/ambroise-leclerc/WebFront/issues/449) Stockage modifiable et livraison des services | [#460](https://github.com/ambroise-leclerc/WebFront/issues/460) stockage sous racines déclarées, confiné pendant la résolution (révision de l'ADR-002) ; [#461](https://github.com/ambroise-leclerc/WebFront/issues/461) paquet `@webfront/services` | sélecteur HTML et écritures (règles 2 à 4) |
| 5 | [#450](https://github.com/ambroise-leclerc/WebFront/issues/450) Intégration native des frontends | [#462](https://github.com/ambroise-leclerc/WebFront/issues/462) exécuteur du thread d'interface, y compris le navigateur système ; [#463](https://github.com/ambroise-leclerc/WebFront/issues/463) fenêtre native et correspondance avec les connexions | backend natif seulement |

Après l'étape 4, le premier service (sélecteur HTML, poignées, import et export côté client) fera l'objet de sa propre
épique ; les dialogues natifs suivront l'étape 5. `PUT`/`POST` ne sont pas des préalables : les écritures passent par
des tableaux typés en morceaux ; leur ajout au serveur, utile aux très gros envois en flux et aux formulaires sans
JavaScript vers un équipement, relèvera de sa propre décision.

## Autres services du système

Règle de tri : **si la plateforme web fournit le service dans tous les frontends et toutes les configurations visées,
WebFront n'ajoute rien** ; un service C++ n'existe que si le navigateur ne peut pas le rendre (ressource de l'hôte),
s'il le rend mal hors contexte sécurisé, ou si le client est distant. Chaque service suit les règles 1 à 3 (facultatif,
autorisations explicites, backend par connexion).

| Priorité | Service | Navigateur suffisant ? | Backends | Contraintes embarquées / remarques |
| --- | --- | --- | --- | --- |
| 1 | **Fichiers de l'hôte** : ouvrir, enregistrer, choisir un dossier, parcourir | non | natif, HTML, client | décrit ci-dessus |
| 1 | **Répertoires standard** : données, configuration, cache, journaux, documents, temporaire | non | C++ seul (`SHGetKnownFolderPath`, `NSSearchPathForDirectoriesInDomains`, XDG, `getFilesDir`) | sur cible embarquée, fournis par l'intégration (partition de données) et jamais devinés ; prérequis des racines du sélecteur HTML |
| 1 | **Ouvrir un lien ou un fichier avec l'application par défaut**, révéler dans le gestionnaire de fichiers | partiellement (`target=_blank` dans le navigateur système) | natif (`ShellExecuteExW`, `NSWorkspace`, portail `OpenURI`/`xdg-open`) | **dangereux** : liste des schémas autorisés par l'application, jamais d'exécutable ; indisponible pour un client distant ; nécessaire aux frontends intégrés, qui bloquent les fenêtres surgissantes (ADR-008) |
| 2 | **Presse-papiers** (texte, puis images) | oui en contexte sécurisé, sur geste utilisateur | navigateur ; C++ (Win32, `NSPasteboard`, portail/GTK) pour copier depuis C++ ou hors contexte sécurisé | pour un client distant en HTTP clair, seul `document.execCommand` reste, sans lecture ; le presse-papiers de l'hôte n'a alors aucun sens |
| 2 | **Boîtes de message** (alerte, confirmation) | oui (`<dialog>`, `confirm()`) | navigateur ; natif uniquement pour un message émis sans page (erreur de démarrage) | WKWebView exige `runJavaScriptAlertPanel…` pour `alert`/`confirm` : même préalable que la règle 6 |
| 2 | **Notifications** | oui en contexte sécurisé, avec permission ; non si la fenêtre est fermée | natif (toasts Windows liés à l'AUMID, `UNUserNotificationCenter`, portail `Notification`) | sur équipement, une notification va à l'IHM connectée, pas au système ; aucune hors connexion |
| 2 | **Glisser-déposer depuis le système** avec chemins de l'hôte | fichiers du client seulement | adaptateur (WebView2 `PostWebMessageAsJsonWithAdditionalObjects`, `NSDraggingDestination`, GTK) | produit des autorisations comme un dialogue |
| 2 | **Fenêtres** : titre, taille, plein écran, plusieurs fenêtres, fermeture contrôlée | partiellement (Fullscreen API) | adaptateur (ADR-007, ADR-017) | kiosque : plein écran forcé à l'intégration |
| 3 | **Menus natifs, barre système, raccourcis globaux** | non | adaptateur | macOS impose une barre de menus ; inexistants en kiosque et à distance |
| 3 | **Instance unique, associations de fichiers, liens profonds** | non | C++ et installation | dépendent de l'empaquetage (MSIX, bundle, `.desktop`) |
| 3 | **Stockage sécurisé de secrets** | non (`localStorage` n'est pas un coffre) | Gestionnaire d'identification Windows, Keychain, libsecret/portail `Secret` | sur équipement : élément sécurisé, TPM ou partition chiffrée, à l'intégration |
| 3 | **Inhibition de la veille et de l'écran de veille** | oui (Screen Wake Lock, contexte sécurisé, Chromium et WebKit récents) | natif (`SetThreadExecutionState`, `IOPMAssertion`, portail `Inhibit`) | essentiel en kiosque |
| 3 | **Supports amovibles** : apparition, montage, éjection | non | udisks2/udev, `WM_DEVICECHANGE`, `DiskArbitration` | central sur équipement (mise à jour, export de journaux) ; alimente les racines du sélecteur HTML |
| 3 | **Impression et export PDF** | oui (`window.print()`) | adaptateur (WebView2 `PrintToPdf`, CEF `PrintToPDF`, WebKit) | équipement sans imprimante : export vers un fichier ou le client |
| 3 | **Informations système** : plateforme, version, langue et thème de l'hôte, état réseau, batterie | langue et thème du **client** oui | C++ | pour un client distant, langue et thème du client et de l'hôte diffèrent : les deux sont exposés distinctement |
| 4 | **Périphériques** : série, USB, Bluetooth, GPIO | Web Serial/USB/Bluetooth : Chromium seul, contexte sécurisé, côté client | C++ | sur équipement, les périphériques sont ceux de l'hôte : services propres à l'application plutôt que génériques |
| 4 | **Démarrage à l'ouverture de session, redémarrage/arrêt, date et heure de l'hôte** | non | C++ et intégration système | privilèges élevés : jamais par défaut, toujours sous autorisation explicite |
| — | **Lancer des processus, exécuter des commandes** | non | — | **écarté** comme service générique : à exposer, s'il le faut, sous forme de fonctions métier de l'application |

## Critères d'acceptation

0. **Préalables** : les épiques #446 à #450 nécessaires au backend concerné sont terminées.
1. **Comportements web standard** : `<input type="file">`, `alert`/`confirm` et un téléchargement fonctionnent dans CEF, WebView2,
   WKWebView et WebKitGTK ; scénario de la suite partagée, sélection simulée par le moteur quand il le permet.
2. **Sélecteur HTML** : listage paginé borné, refus de toute sortie des racines (`..`, liens symboliques, chemins
   absolus, noms Windows réservés), racine `none` inaccessible depuis la page, racine `read` refusant toute écriture
   même par un appel direct sans le sélecteur, poignée inutilisable par une autre connexion, jeton expiré refusé, écriture
   atomique ; **remplacement concurrent** d'un composant par un lien vers l'extérieur de la racine pendant la lecture,
   l'écriture, la création, le renommage et la suppression, sans qu'aucune opération n'atteigne l'extérieur ; tests Catch2 avec système de fichiers simulé, puis test navigateur piloté par l'automatisation.
3. **Choix du backend** : par défaut, connexion non associée à la vue d'un frontend → jamais natif, y compris depuis
   la boucle locale (tunnel, autre utilisateur) ; avec l'option de confiance explicite, connexion de boucle locale →
   natif possible, connexion distante → toujours HTML ; sans racine → pas de HTML ; annulation → liste vide ; aucun
   backend → erreur documentée côté C++ et JavaScript ; backends simulés pour les tests.
4. **Natif** : un backend au moins par plateforme de bureau (portail XDG sous Linux), vérifié manuellement et par un
   test de construction ; dialogue sur le thread de l'interface, parent connu avec un frontend intégré ; fermeture de
   l'application pendant un dialogue ouvert sans blocage ni accès après destruction.
5. **Lecture par jeton** : `Cache-Control: no-store`, `Referrer-Policy: no-referrer`, jeton absent des journaux ;
   lecture distante en HTTP clair refusée sans `allowPlaintextRemote` ; `Range`, MIME, refus après fermeture de la connexion, pas de fuite de chemin dans l'URL,
   mémoire du serveur indépendante de la taille du fichier servi ; plusieurs requêtes `Range` successives avec le même
   jeton, positionnement et reprise d'une `<video>`, renouvellement avant expiration ; même URL rejouée depuis un
   autre client (autre profil ou navigateur, client HTTP) sans le cookie refusée, acceptée seulement en mode
   transférable ; rejouée depuis une autre page du même profil, acceptée (garantie limitée à la possession du cookie,
   documentée) ; refusée après fermeture de la connexion propriétaire ; poignée refusée par le pont sur une autre
   connexion.
6. **Ressources** : aucune ressource ajoutée au cœur (cas `MinimalCoreAssets`) ; sélecteur HTML chargé seulement si le
   service est instancié.
7. Documentation `docs/SERVICES.md` : backends par configuration, limites, absence d'isolation (ADR-008 inchangée).

## Options écartées et motifs

- **Une seconde fenêtre WebView pour le sélecteur** : impossible avec le navigateur système et à distance, lourde en
  embarqué, sans gain d'isolation (même origine, même pont) et sans aspect natif ; elle cumule les défauts des
  solutions A et B.
- **Un sélecteur propre à React** : React est facultatif (ADR-010, `docs/REACT.md`) ; le sélecteur HTML est un élément
  personnalisé que React, ou toute autre bibliothèque, peut envelopper.
- **Une API générique de fichiers pour la page** (`fs.readFile(path)`), comme `nodeIntegration` ou `filesystem.*` sans
  portée : donnerait à tout script admis et à tout client distant le pouvoir de l'utilisateur de l'hôte.
- **Les seules API du navigateur** : la File System Access API n'existe pas dans WebKit, et aucune API web ne désigne
  les fichiers de l'hôte quand il est distant.
- **Les seuls dialogues natifs** : faux pour un client distant, impossible en kiosque, non automatisable.
- **Lancer `zenity`, `kdialog` ou `osascript`** : dépend des programmes installés, sensible à l'injection de commandes.
- **Dépendre de Qt, GTK ou d'une bibliothèque compilée dans le cœur** : contraire au cœur en en-têtes (ADR-001) et au
  parcours par défaut sans dépendance (ADR-007) ; reste possible derrière le concept de backend, côté application.
- **Transporter le contenu des fichiers en chaînes du pont** : une chaîne est bornée à 64 Kio par son en-tête (pas par
  WebSocket), un message entier à 64 Mio, et la conversion coûte en mémoire ; la lecture HTTP en flux et l'écriture par
  tableaux typés en morceaux conviennent.
- **Déduire la session locale de l'adresse réseau** : un tunnel, un proxy local ou un autre utilisateur de la machine
  arrivent sur la boucle locale ; seule l'association explicite à la vue d'un frontend fait foi.

## Conséquences

- **Bénéfice :** une seule API pour les configurations bureau, kiosque, distante et, plus tard, mobile ; un sélecteur
  testable et automatisable ; des autorisations conformes aux bacs à sable modernes.
- **Bénéfice :** le préalable de la règle 6 corrige un défaut existant des frontends intégrés, indépendamment du reste.
- **Coût accepté :** code natif par plateforme (COM, Objective-C, D-Bus) et matrice de tests manuels pour les
  dialogues ; un sélecteur HTML à maintenir, accessible au clavier et aux lecteurs d'écran.
- **Coût accepté :** l'application doit déclarer ses racines et ses autorisations ; rien ne fonctionne « par magie »
  sur l'arborescence de l'hôte.
- **Risque :** le contrôle d'origine (ADR-008) reste la seule frontière du pont ; un script admis peut ouvrir des
  dialogues et lire les fichiers accordés. Les autorisations limitent la portée, pas la confiance.
- **Risque :** une racine accessible à la page l'est pour tout script admis, sans sélection par l'utilisateur ; le
  sélecteur HTML ne l'empêche pas. Seules les racines `none` et le dialogue natif réservent l'accès à une sélection
  réelle.
- **Coût accepté :** la liaison HTTP vaut pour un profil de navigateur : les pages de même origine d'un même profil
  peuvent lire les URL les unes des autres.
- **Coût accepté :** un cookie de liaison par client, à qualifier dans chaque moteur ; les téléchargements gérés par
  l'adaptateur doivent le présenter.
- **Risque :** WebFront ne chiffre pas ; un client distant sans transport chiffré fourni par le déploiement expose
  jeton et contenu. La lecture distante en clair est refusée sauf acceptation explicite.
- **Risque :** en mode transférable, toute personne qui obtient l'URL lit le fichier pendant sa validité ; ce mode est
  explicite, de courte durée et absent par défaut.
- **Risque :** la configuration de confiance du navigateur système réintroduit la déduction par adresse ; elle reste
  explicite, désactivée par défaut et documentée comme non sûre. Sans association, le défaut est HTML.

## Prescriptions d'implémentation et de génération de code

- Ne jamais présenter le sélecteur HTML comme un contrôle d'autorisation : vérifier l'accès déclaré de la racine dans
  chaque opération C++, quel que soit l'appelant.
- Ne jamais transmettre à JavaScript un chemin de l'hôte sans demande explicite de l'application ; ne jamais accepter
  de JavaScript un chemin à ouvrir : seulement une poignée émise pour cette connexion.
- `SafePath` ne valide que la syntaxe d'un chemin relatif. Le confinement à une racine se vérifie **pendant** la
  résolution, par des opérations relatives à un répertoire racine déjà ouvert, jamais par une vérification du chemin
  suivie d'une ouverture par nom (fenêtre de course : un processus local peut remplacer un composant par un lien entre
  les deux). Linux : `openat2` avec `RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS` (noyau 5.6+), sinon descente composant
  par composant avec `openat(O_NOFOLLOW | O_DIRECTORY)` ; écritures par `openat(O_CREAT | O_EXCL)` dans le répertoire
  ouvert, puis `renameat`, `unlinkat`, `mkdirat` relatifs à ce même descripteur. macOS : même descente avec
  `O_NOFOLLOW` (ou `O_NOFOLLOW_ANY`), `renameatx_np`. Windows : ouvertures relatives au handle du répertoire
  (`NtCreateFile` avec `RootDirectory`), points de réanalyse refusés par composant (`FILE_FLAG_OPEN_REPARSE_POINT`),
  renommage par `SetFileInformationByHandle` sur le handle ouvert. Un lien interne à la racine est suivi seulement si la
  résolution le confine ; par défaut, les liens sont refusés. Les stockages sans liens (FatFs, LittleFS) se contentent du
  contrôle syntaxique.
- Ne jamais fonder une autorisation sur l'adresse de la connexion ; seule l'association à la vue d'un frontend, ou la
  configuration de confiance explicite, rend le backend natif utilisable.
- Servir le contenu des fichiers en flux borné, jamais par copie complète en mémoire, avec `Cache-Control: no-store`
  et `Referrer-Policy: no-referrer` ; ne jamais journaliser un jeton.
- Ouvrir les dialogues natifs sur le thread de l'interface, avec le parent de l'adaptateur quand il existe ; la tâche
  qui attend le résultat est possédée et annulable (ADR-006).
- Préférer le portail XDG sous Linux ; ne pas lancer de programme externe pour afficher un dialogue.
- Ne pas ajouter de ressource au cœur ni de dépendance au parcours par défaut ; un backend natif est une option CMake.
- Fournir pour chaque service un backend simulé utilisé par les tests Catch2 et Jasmine.
- Un nouveau service commence par la règle de tri ci-dessus et justifie pourquoi le navigateur ne suffit pas.

## Sources et vérification

- WebFront : [ADR-002](ADR-002-systemes-de-fichiers-composables.md), [ADR-003](ADR-003-protocole-binaire-et-bornes.md),
  [ADR-006](ADR-006-taches-structurees-et-arret.md), [ADR-007](ADR-007-frontends-explicites.md),
  [ADR-008](ADR-008-contenu-de-confiance-et-origines.md), [ADR-010](ADR-010-composants-et-outils-optionnels.md),
  [ADR-016](ADR-016-bootstrap-comme-socle.md), [ADR-017](ADR-017-acces-direct-aux-frontends.md),
  [ADR-019](ADR-019-developpement-automatisable-et-agents.md) ;
  [HTTPServer](../../include/webfront/http/HTTPServer.hpp) (`GET`/`HEAD`), [Messages](../../include/webfront/weblink/Messages.hpp)
  (`maxStringBytes`), [SafePath](../../include/webfront/system/SafePath.hpp), adaptateurs sous
  [frontend/](../../include/webfront/frontend/) (aucun traitement de `<input type="file">` au 2026-10-31).
- Electron : [dialog](https://www.electronjs.org/docs/latest/api/dialog), [contextBridge](https://www.electronjs.org/docs/latest/api/context-bridge),
  [webUtils](https://www.electronjs.org/docs/latest/api/web-utils).
- Tauri v2 : [capabilities](https://v2.tauri.app/reference/acl/capability), [permissions](https://v2.tauri.app/security/permissions/),
  [scopes](https://v2.tauri.app/security/scope/), [greffon dialog](https://v2.tauri.app/plugin/dialog/).
- Wails : [runtime dialog](https://wails.io/docs/reference/runtime/dialog). Neutralinojs : [os](https://neutralino.js.org/docs/api/os),
  [configuration](https://neutralino.js.org/docs/configuration/neutralino.config.json).
- saucer : [dépôt](https://github.com/saucer/saucer), [modules](https://saucer.app/docs/advanced/modules).
- Qt : [QFileDialog](https://doc.qt.io/qt-6/qfiledialog.html), [QWebEnginePage::chooseFiles](https://doc.qt.io/qt-6/qwebenginepage.html#chooseFiles).
- Portail XDG : [FileChooser](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html),
  [portails Flatpak](https://github.com/flatpak/flatpak/wiki/Portals).
- Navigateur : [ouvrir des fichiers et prise en charge de `showOpenFilePicker`](https://web.dev/patterns/files/open-one-or-multiple-files),
  [File System Access API](https://developer.chrome.com/articles/file-system-access/).
- Rust : [rfd](https://docs.rs/rfd). C/C++ : [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended),
  [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs).
- Embarqué : [LVGL file explorer](https://docs.lvgl.io/master/details/auxiliary-modules/file_explorer.html),
  [exemple file_server d'ESP-IDF](https://github.com/espressif/esp-idf/tree/master/examples/protocols/http_server/file_serving).
- Mobile : [Storage Access Framework](https://developer.android.com/training/data-storage/shared/documents-files),
  [UIDocumentPickerViewController](https://developer.apple.com/documentation/uikit/uidocumentpickerviewcontroller),
  [signets « security-scoped »](https://developer.apple.com/documentation/foundation/nsurl/bookmarkcreationoptions/withsecurityscope).

Les sources externes ont été consultées le 2026-10-31 ; la prise en charge de `showOpenFilePicker` hors Chromium est
celle publiée par web.dev à cette date et doit être revérifiée lors de l'implémentation. Aucun backend n'est encore
mesuré ni testé : les capacités des moteurs citées sont à qualifier par la suite partagée.
