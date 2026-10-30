# ADR-017 : Accéder aux frontends par leur API directe sans détrôner le chemin HTML

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-25 | Proposée | Ambroise Leclerc | Décision initiale (#363) : transport direct facultatif par adaptateur, format de l'ADR-003 inchangé, HTML normatif ; mesures WebKitGTK, WebView2 (tampon partagé) et WKWebView, contraintes des API publiques Cocoa, évaluation du défaut par plateforme et correction de deux coûts du chemin chaud du bridge. | À renseigner après intégration. |
| 1.1 | 2026-10-25 | Proposée | Ambroise Leclerc | Distingue disponibilité du chemin HTML et défaut par plateforme, précise les règles réseau mobiles par version et renouvelle les mesures Linux avec la passe CI Release dédiée. | À renseigner après intégration. |

## État et chronologie

**Proposée** le **2026-10-24**, mise à jour le **2026-10-25** (issue #363). Cette décision prépare l'ajout de nouveaux frontends, notamment Android,
iOS et d'autres plateformes mobiles. Elle ne modifie ni l'API publique ni le format binaire : le seul code livré est
un benchmark et la correction de deux coûts qu'il a révélés dans le chemin HTML. Elle passera à « Acceptée » quand
les critères de la section dédiée seront démontrés.

## Contexte

Aujourd'hui, tous les frontends suivent le même chemin : le serveur embarqué sert la page par HTTP sur la boucle
locale, puis `WebFront.js` ouvre un WebSocket vers ce même serveur et y échange les messages binaires de
l'[ADR-003](ADR-003-protocole-binaire-et-bornes.md). Les adaptateurs de l'[ADR-007](ADR-007-frontends-explicites.md)
(CEF, WebView2, WKWebView, WebKitGTK) n'ouvrent qu'une fenêtre sur une URL `http://127.0.0.1:<port>/`.

Ce chemin HTML est le cœur de WebFront et doit le rester :

- c'est le seul qui fonctionne avec un navigateur quelconque, local ou distant, sans code propre au moteur ;
- l'évolution visée est de supporter des micro-contrôleurs très légers qui servent leur interface à un navigateur
  distant (téléphone, poste) : aucun frontend local n'y est envisageable, seul le HTML servi par HTTP/WebSocket existe ;
- le navigateur système, défaut de `webfront::WebFront`, n'offre aucune autre voie.

Les moteurs embarqués offrent pourtant tous une API directe, qui ne passe ni par un socket ni par HTTP :

| Moteur | Ressources | JavaScript → natif (avec réponse) | Natif → JavaScript |
| --- | --- | --- | --- |
| WebKitGTK 6.0 | `webkit_web_context_register_uri_scheme` | message handler avec réponse (2.40+) | `webkit_web_view_evaluate_javascript` |
| WKWebView (macOS, iOS) | `WKURLSchemeHandler` | `WKScriptMessageHandlerWithReply` (macOS 11, iOS 14) | `evaluateJavaScript`, `callAsyncJavaScript` |
| WebView2 | `SetVirtualHostNameToFolderMapping`, `WebResourceRequested` | `chrome.webview.postMessage`, `WebMessageReceived` | `PostWebMessageAsJson`/`AsString`, `ExecuteScriptAsync` |
| CEF | `CefSchemeHandlerFactory` | `CefMessageRouter`, messages de processus | `ExecuteJavaScript`, messages de processus |
| Android WebView | `WebViewAssetLoader`, `shouldInterceptRequest` | `WebViewCompat.addWebMessageListener` | `postWebMessage`, `evaluateJavascript` |

Sur mobile, un serveur local n'est pas aisé :

- Les règles de HTTP en clair dépendent de la version du système et de la cible : Android désactive ce trafic par
  défaut pour les applications ciblant l'API 28 ou plus, mais Android 17 (API 37) ajoute une configuration implicite
  autorisant la boucle locale lorsqu'aucune configuration ne la vise explicitement. Sur iOS 10 à 16, ATS autorise les
  connexions aux adresses IP sans exception ; à partir d'iOS 17, une exception ATS est nécessaire pour ces adresses
  (`NSAllowsLocalNetworking` ou une exception ciblée dans `NSExceptionDomains`). Ce n'est donc pas une interdiction
  universelle du serveur local ; le prototype devra qualifier les versions prises en charge (sources en fin d'ADR).
  Voir aussi [WKWEBVIEW](../WKWEBVIEW.md#bundles-and-local-networking) ; ces mesures macOS ne prouvent pas le comportement iOS.
- Le système suspend l'application en arrière-plan : les sockets d'écoute et les connexions ne survivent pas
  forcément au retour au premier plan, et le cycle de vie du serveur se distingue alors de celui de la vue.
- Un port de la boucle locale est accessible à toute autre application du même appareil. Le contrôle d'origine de
  l'[ADR-008](ADR-008-contenu-de-confiance-et-origines.md) filtre les navigateurs, pas un processus natif qui forge
  l'en-tête `Origin` : sur un appareil partagé entre applications, un canal propre à la vue réduit cette exposition.

## Mesures (WebKitGTK, WebView2 et WKWebView, 2026-10-24/25)

Le benchmark `test/web/WebKitGTKTransportBenchmark.cpp`, cible manuelle
`webkitgtk_transport_benchmark`, ouvre une seule fenêtre WebKitGTK servie par WebFront et exécute les mêmes
échos de chaînes par les deux chemins (`test/web/transport-benchmark.html`) :

- **HTML** : `webFront.cppFunction("echo")` pour JavaScript → C++, `ui.jsFunctionSender<std::string>` pour
  C++ → JavaScript, chaque appel attendant le précédent ;
- **direct** : un message handler avec réponse (`window.webkit.messageHandlers.wfecho.postMessage`) et
  `webkit_web_view_evaluate_javascript`, enregistrés par le benchmark sur la vue de l'adaptateur avant la navigation ;
- « concurrent » : 64 appels JavaScript → C++ en vol, ce qui mesure le débit plutôt qu'un aller-retour.

Chaque valeur est la médiane, sur 5 exécutions précédées d'un échauffement, du temps moyen par appel, puis la médiane
de trois lancements du programme (`ctest -R WebKitGTKTransportBenchmark` trois fois, en conservant chaque rapport JSON).
Machine : AMD Ryzen 9 3900X (24 threads), Ubuntu 26.04.1 LTS, Linux 7.0.0-38-generic, GCC 16.1 en Release
sans couverture, WebKitGTK 2.52.6, Xvfb avec rendu logiciel (`GSK_RENDERER=cairo`). La page partagée utilise les mêmes
8000 appels par lot (800 à 32 Kio) que Windows. La résolution de `performance.now()` de WebKit est d'environ une
milliseconde ; le lot le plus court de chaque lancement a duré de 86 à 96 ms.
Les [trois rapports bruts et leur médiane](../benchmarks/linux-webkitgtk-2026-10-25.json) sont conservés avec la configuration.
Ces mesures remplacent celles du 2026-10-24, réalisées avec 1000 appels (200 à 32 Kio) et des lots trop courts de 12 à 17 ms.

| Sens | Mode | Octets | Appels | HTML (µs/appel) | Direct (µs/appel) | HTML/direct |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| JS → C++ | séquentiel | 16 | 8000 | 125,8 | 86,9 | 1,4 |
| JS → C++ | concurrent | 16 | 8000 | 26,5 | 12,0 | 2,2 |
| C++ → JS | séquentiel | 16 | 8000 | 131,6 | 69,8 | 1,9 |
| JS → C++ | séquentiel | 1024 | 8000 | 139,5 | 92,5 | 1,5 |
| JS → C++ | concurrent | 1024 | 8000 | 33,6 | 17,0 | 2,0 |
| C++ → JS | séquentiel | 1024 | 8000 | 153,6 | 74,8 | 2,1 |
| JS → C++ | séquentiel | 32768 | 800 | 703,8 | 401,2 | 1,8 |
| JS → C++ | concurrent | 32768 | 800 | 293,8 | 126,2 | 2,3 |
| C++ → JS | séquentiel | 32768 | 800 | 731,5 | 367,5 | 2,0 |

Le premier appel possible survient de 7 à 8 ms après le début de la navigation par le bridge (connexion WebSocket
établie à 7 ms), de 6 à 7 ms par le canal direct. Dans les deux cas la page elle-même vient du serveur HTTP : ce
chiffre ne mesure pas un démarrage sans serveur, que seul un schéma de ressources natif permettrait.

Lecture :

- Le chemin HTML ajoute de 39 à 79 µs par aller-retour séquentiel jusqu'à 1 Kio (302 à 364 µs à 32 Kio) et divise
  le débit concurrent par 2,0 à 2,3. Pour une
  interface (une image à 60 Hz dure 16,7 ms), les deux chemins tiennent largement le budget : **la performance seule
  ne justifie pas d'abandonner le HTML sur poste de travail**. Le canal direct se justifie par les contraintes de
  plateforme et d'exposition ci-dessus, et pour des flux à haut débit.
- L'écart mesuré majore le gain d'un futur transport direct : le benchmark répond sur le thread GTK, sans le codec
  de l'ADR-003 ni le passage au contexte d'exécution du serveur qu'un vrai transport ajoutera. Ces deux coûts sont à
  mesurer avec le même benchmark lors de l'implémentation.
- Le benchmark a révélé deux coûts dans le chemin chaud du bridge, corrigés avec des tests de régression : chaque
  encodage de paramètre écrivait sur `std::cout` (`Messages.hpp`), et `Frame::addBuffer` formatait un dump
  hexadécimal de chaque tampon envoyé avant que `log::debug()` ne teste le niveau (21,7 Mo alloués pour un tampon de
  1 Mio). Lors des mesures initiales du 2026-10-24, avant correction, l'écho de 32 Kio coûtait 2380 µs (JS → C++) et 2528 µs (C++ → JS) par le chemin HTML,
  soit 6 fois le chemin direct.
- Non mesurés dans ce tableau : CEF, WKWebView, Android et iOS. Aucune extrapolation n'est faite ; les mesures WKWebView
  sont présentées séparément ci-dessous, les autres font partie des
  critères d'acceptation. Les mesures WebView2 ci-dessous montrent qu'on ne peut pas extrapoler d'un moteur à l'autre.

### Mesures WebView2 (Windows)

`test/web/WebView2TransportBenchmark.cpp` (cible manuelle `webview2_transport_benchmark`) exécute la même page
dans une fenêtre WebView2, avec les mêmes échos. Le canal direct est enregistré sur le navigateur de l'adaptateur avant la
navigation (observateur interne `Session::viewReady`) :

- **direct** (chaînes) : `chrome.webview.postMessage` / `WebMessageReceived` / `PostWebMessageAsString` pour
  JavaScript → C++ (WebView2 n'a pas de réponse par appel : l'appel porte un identifiant) et `ExecuteScript` pour C++ → JavaScript ;
- **partagé** (octets) : un `ICoreWebView2SharedBuffer` de 128 Kio partagé avec la page (`PostSharedBufferToScript`),
  une moitié écrite par JavaScript, l'autre par C++ ; les messages ne disent que quelle moitié est prête. Séquentiel
  seulement : le tampon n'a qu'un propriétaire à la fois. Chaque côté vérifie tous les octets reçus.

Machine : CurmudgeonWindows, Windows 11 (10.0.22631), MSVC, Release, WebView2 Evergreen 155.0.4283.62, SDK 1.0.3537.50.
Même méthode : médiane de 5 exécutions, puis médiane de trois lancements du programme ; le lot le plus court a duré
82 à 86 ms.

| Sens | Mode | Octets | Appels | HTML (µs/appel) | Direct, chaînes | HTML/direct | Partagé | HTML/partagé |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| JS → C++ | séquentiel | 16 | 8000 | 180 | 91 | 2,0 | 92 | 2,0 |
| JS → C++ | concurrent | 16 | 8000 | 51 | 12 | 4,3 | — | — |
| C++ → JS | séquentiel | 16 | 8000 | 178 | 225 | 0,8 | 87 | 2,0 |
| JS → C++ | séquentiel | 1024 | 8000 | 192 | 106 | 1,8 | 94 | 2,0 |
| JS → C++ | concurrent | 1024 | 8000 | 52 | 21 | 2,5 | — | — |
| C++ → JS | séquentiel | 1024 | 8000 | 191 | 246 | 0,8 | 87 | 2,2 |
| JS → C++ | séquentiel | 32768 | 800 | 651 | 953 | 0,7 | 112 | 5,8 |
| JS → C++ | concurrent | 32768 | 800 | 344 | 484 | 0,7 | — | — |
| C++ → JS | séquentiel | 32768 | 800 | 672 | 1210 | 0,6 | 106 | 6,3 |

Le premier appel possible survient 90 à 109 ms après le début de la navigation par le bridge (connexion prête à 89 à
108 ms), 83 à 98 ms par le canal direct : ici encore la page vient du serveur HTTP.

Lecture :

- Sous WebView2, le canal de messages en chaînes **n'est pas plus rapide que le HTML** dès que les messages grossissent
  (JSON/UTF-16 aux deux extrémités, 1,4 à 1,8 fois plus lent à 32 Kio) ni pour C++ → JavaScript
  (`ExecuteScript` réanalyse un script à chaque appel). Il ne gagne que pour les petits appels JavaScript → C++
  (un facteur 2) et en débit concurrent de petits messages (2,5 à 4,3).
- Seul le tampon partagé, qui évite toute conversion par une chaîne, est partout plus rapide et presque indépendant de
  la taille (de 87 à 112 µs de 16 octets à 32 Kio), soit 2 fois le HTML pour les petits messages et 6 fois à 32 Kio. Un
  transport direct WebView2 doit donc être conçu autour de lui, pas autour de `PostWebMessageAsString`. Ce
  benchmark mesure l'échange d'octets, pas encore le codec ni le changement de thread (voir la lecture WebKitGTK).
- À l'inverse, WebKitGTK gagne même avec des chaînes. Le gain d'un canal direct dépend donc du moteur, de la
  forme du canal et de la taille, et se mesure par adaptateur.

### Mesures WKWebView (macOS, 2026-10-25)

`WKWebViewTransportBenchmark` ouvre la même page et le même adaptateur AppKit que la suite Jasmine, dans un bundle
agent. Machine : Apple M2, 16 Gio, macOS 27.0.1 (26A434), WebKit système 22625.1.29.11.28, SDK 27.0, Clang 21.1.8,
Release. La charge des autres applications du poste n'est pas contrôlée ; aucune compilation ni autre suite CTest ne
tourne pendant les mesures. Ce n'est pas une comparaison de machines avec les chiffres Linux ou Windows.

Les appels et tailles sont ceux de la page commune (8000 appels à 16 octets et 1 Kio, 800 à 32 Kio, concurrence 64).
Chaque chiffre est la médiane de trois lancements, chacun prenant la médiane de cinq séries après échauffement :

- **direct** : `WKScriptMessageHandlerWithReply` pour JavaScript → C++, `evaluateJavaScript` pour C++ → JavaScript ;
- **callAsync** : C++ → JavaScript avec arguments Foundation séparés du corps de fonction, dans `pageWorld` ;
- **base64** : les mêmes octets codés/décodés à chaque appel dans les deux sens (`btoa`/`atob` côté JS,
  `NSData` côté C++), transportés par un handler avec réponse ou par `callAsyncJavaScript`.

Les rapports JSON bruts et leurs médianes sont conservés dans
[wkwebview-transport-2026-10-25.json](../benchmarks/wkwebview-transport-2026-10-25.json).
La base64 est donc incluse dans sa colonne, mais le codec de l'ADR-003 et le changement de thread restent exclus des
trois chemins natifs. Les échos de chaînes sont ASCII ; la sonde séparée vérifie aussi les 256 valeurs d'octet en base64.

| Sens | Mode | Octets | Appels | HTML (µs/appel) | Direct | callAsync | Base64 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| JS → C++ | séquentiel | 16 | 8000 | 134,1 | 61,8 | — | 62,6 |
| JS → C++ | concurrent | 16 | 8000 | 34,4 | 6,0 | — | 6,6 |
| C++ → JS | séquentiel | 16 | 8000 | 135,3 | 45,3 | 61,5 | 66,1 |
| JS → C++ | séquentiel | 1024 | 8000 | 155,2 | 72,8 | — | 86,4 |
| JS → C++ | concurrent | 1024 | 8000 | 38,1 | 8,1 | — | 16,4 |
| C++ → JS | séquentiel | 1024 | 8000 | 151,7 | 54,9 | 73,0 | 86,3 |
| JS → C++ | séquentiel | 32768 | 800 | 337,5 | 106,2 | — | 387,5 |
| JS → C++ | concurrent | 32768 | 800 | 108,8 | 23,8 | — | 226,2 |
| C++ → JS | séquentiel | 32768 | 800 | 334,1 | 85,5 | 103,5 | 392,7 |

Les lots les plus courts durent 17 à 59 ms : avec une horloge JS d'environ 1 ms, l'incertitude de quantification peut
atteindre 6 % sur les lots concurrents de 32 Kio. Le premier lancement a duré 157 s, les suivants 76 et 81 s ; certains
chemins ont fortement varié (C++ → JS HTML à 16 octets : 135 à 791 µs). Les trois rapports et les minima/maxima par
chemin sont conservés, sans écarter le premier. Ces médianes sont indicatives ; une décision de transport exige des
mesures du transport complet sur un poste à charge contrôlée.

Lecture :

- Les chaînes natives gagnent dans les deux sens sur cette machine. `callAsyncJavaScript` est ici plus lent
  qu'`evaluateJavaScript` ; son intérêt est le passage d'arguments et l'attente des Promises, pas un gain garanti.
- La base64 conserve un gain pour les petits messages, mais l'efface à 32 Kio : environ 15 % de plus que le HTML
  en JavaScript → C++ séquentiel, 18 % en C++ → JavaScript et 2,1 fois le temps par appel en concurrent. Comparer
  seulement les chaînes natives ferait donc recommander à tort un défaut direct pour les flux binaires importants.
- La première réponse intervient en médiane 18 ms après le début de navigation par le HTML (bridge prêt à 17 ms)
  et 8 ms par le handler natif. La page est toujours servie en HTTP : aucun démarrage sans serveur n'est mesuré.
- **macOS reste par défaut en HTTP/WebSocket.** Un transport direct WKWebView peut servir les besoins mobiles ou de
  petits messages, mais doit d'abord porter le format existant, traiter l'enveloppe d'octets et passer la suite entière.
  Ces chiffres ne prouvent ni le fonctionnement ni les performances d'iOS.

### Contraintes des API publiques WKWebView (macOS et iOS)

`WKWebViewTransportContracts` vérifie les réponses Unicode (y compris NUL), les rejets natifs, le rejet d'une réponse
abandonnée, les exceptions et échos incorrects côté JavaScript, l'attente d'une Promise par `callAsyncJavaScript`,
et le refus des appels depuis des sous-cadres de même origine ou d'origine opaque. Ce sont des sondes d'API,
pas encore une implémentation du transport de l'ADR-003. L'examen des API Apple impose les précisions suivantes :

- **Porteur de messages.** Les types documentés pour le corps d'un `WKScriptMessage`, ses réponses et les arguments de
  `callAsyncJavaScript` sont des valeurs Foundation (chaînes, nombres, dates, tableaux, dictionnaires et null).
  `NSData`/`ArrayBuffer` et un tampon partagé analogue à WebView2 ne font pas partie de ce contrat public. Le premier
  transport WKWebView doit donc porter les octets de l'ADR-003 en **base64**, dans les deux sens, et mesurer ce coût ;
  un tableau de nombres constituerait une autre enveloppe à mesurer, pas un type filaire supplémentaire.
- **Asynchronisme.** `callAsyncJavaScript` accepte des arguments séparés du corps de fonction et attend la Promise
  retournée. `evaluateJavaScript` ne fournit pas cette attente : la sonde renvoie un échec lorsqu'un écho renvoie une
  Promise. Un vrai transport remet les octets au codec JS, qui garde la gestion des appels, erreurs et identifiants de
  l'ADR-003 ; les Promises natives ne remplacent ni ce protocole ni `webFront.ready`. Un reply block conservé doit être
  copié, terminé une seule fois, puis libéré, y compris à la fermeture ; les callbacks tardifs ne gardent pas la session en vie.
- **Cadres et mondes.** Un handler enregistré dans `WKContentWorld.pageWorld` est exposé aux sous-cadres. Le monde
  de contenu n'est pas un contrôle d'origine : vérifier `frameInfo.isMainFrame` et `frameInfo.securityOrigin` pour
  chaque message. La sonde applique ces deux contrôles ; une navigation autorisée reste distincte d'un accès au bridge.
- **Ressources.** `WKURLSchemeHandler` ne peut pas remplacer un schéma que WebKit gère déjà, tel que HTTP(S). Il faut
  un schéma applicatif. Le normaliseur actuel de `ContentPolicy` n'accepte que HTTP(S), et la session WKWebView attend
  une réponse HTTP réussie pour achever son démarrage : ces deux points doivent être adaptés explicitement, sans
  désactiver leurs contrôles pour le chemin HTTP. MIME, modules, origines, stockage, CORS et contexte sécurisé du nouveau
  schéma restent à prouver par la suite partagée ; ce benchmark charge toujours sa page en HTTP.
- **Annulation des ressources.** Après `stopURLSchemeTask`, Apple interdit les callbacks de progression ou de fin sur
  la tâche arrêtée. Le fournisseur doit retirer la tâche de ses opérations vivantes et écarter toute réponse tardive,
  conformément à l'ADR-006. Une ressource directe n'est donc pas un simple alias de `NativeDebugFS` ou d'une réponse HTTP.

Ces API sont disponibles dès macOS 11/iOS 14 pour les messages avec réponse et `callAsyncJavaScript` ; elles ne relèvent
pas le minimum macOS 13.3 du frontend. Elles ne prouvent pas le fonctionnement sur iOS, qui reste à prototyper. Le défaut
macOS reste HTTP tant que le transport complet et les ressources directes ne passent pas les critères d'acceptation.

## Autres possibilités des API natives

Hors des canaux de messages mesurés ci-dessus, les API des moteurs offrent des possibilités que le HTML sur boucle
locale n'a pas. Seule la première (WebView2) est mesurée ici ; les autres sont lues dans la documentation des moteurs et
restent à qualifier par un test avant d'être retenues :

| Possibilité | Moteurs | Intérêt pour WebFront | État |
| --- | --- | --- | --- |
| Mémoire partagée sans copie | WebView2 (`CreateSharedBuffer`, `PostSharedBufferToScript`) ; CEF (messages de processus à région de mémoire partagée, à confirmer pour la version épinglée) | Messages binaires sans chaîne intermédiaire : 6 fois le HTML à 32 Kio sous WebView2 | WebView2 mesuré |
| Messages `ArrayBuffer` | Android (`WebMessageCompat` de type `TYPE_ARRAY_BUFFER`, androidx.webkit 1.8.0 et `WEB_MESSAGE_ARRAY_BUFFER`) ; CEF (`CefV8Value::CreateArrayBuffer`) | Porte les octets de l'ADR-003 sans base64 | à qualifier |
| Script injecté au début du document | WebView2 (`AddScriptToExecuteOnDocumentCreated`), WebKitGTK (`WebKitUserScript`), WKWebView (`WKUserScript`) | Fournit l'objet de canal avant tout script de la page, comme l'exige la décision plus bas | à qualifier |
| Réponse par appel | WebKitGTK 2.40+ et WKWebView (message handler avec réponse) | Un appel JavaScript → C++ n'a pas à gérer lui-même ses identifiants ; WebView2 n'en a pas (mesuré avec un identifiant) | WebKitGTK et WKWebView mesurés |
| Réponses de ressources calculées | `WebResourceRequested` (WebView2), `WKURLSchemeHandler`, `CefSchemeHandlerFactory`, `webkit_web_context_register_uri_scheme`, `shouldInterceptRequest` | Sert `fs::Multi` sans socket, avec les mêmes règles de premier fournisseur | à qualifier |
| Hôte virtuel sur dossier | WebView2 (`SetVirtualHostNameToFolderMapping`) | Plus simple, mais contourne `fs::Multi` : réservé au développement | écarté pour la production |
| Objets hôtes synchrones | WebView2 (`AddHostObjectToScript`) | Aucun : contredit l'asynchronisme de l'[ADR-004](ADR-004-appels-asynchrones-et-erreurs.md) | écarté |

Conclusion de cette comparaison : les gains durables viennent des canaux binaires (tampon partagé, `ArrayBuffer`) et des
ressources servies sans socket, pas du simple remplacement du WebSocket par un canal de chaînes.

## Décision

### Le chemin HTML reste normatif

HTTP pour les ressources et WebSocket pour les messages restent le chemin de référence, complet, disponible et testé
pour tous les frontends, y compris ceux qui offrent un accès direct. Sa disponibilité est obligatoire ; son activation
au démarrage dépend du transport choisi. Aujourd'hui, il est le seul chemin implémenté et le défaut de tous les frontends.
Les futurs défauts par plateforme sont proposés plus bas : un défaut direct n'impose pas de démarrer le serveur HTTP.
Le navigateur système, les clients distants et les futures cibles micro-contrôleurs n'utilisent que lui.
Le cœur (`include/webfront/` hors `frontend/`) n'acquiert
aucune dépendance à un transport direct, et le budget de ressources de l'[ADR-016](ADR-016-bootstrap-comme-socle.md)
ne grossit pas pour lui.

### L'accès direct est une capacité facultative de l'adaptateur

Un adaptateur peut offrir deux capacités, indépendantes et facultatives :

1. **Ressources directes** : servir le même système de fichiers composé (`fs::Multi`, [ADR-002](ADR-002-systemes-de-fichiers-composables.md))
   par le schéma ou l'hôte virtuel natif du moteur, sans socket. Les règles de premier fournisseur gagnant, de
   `WebFront.js` non surchargeable et de page de repli s'appliquent à l'identique.
2. **Messages directs** : transporter les messages binaires de l'ADR-003, octet pour octet, par le canal natif du
   moteur. Le transport n'est qu'un porteur d'octets : `Messages.hpp` et le codec de `WebFront.js` restent les seuls
   codecs. Les octets passent en `ArrayBuffer` ou tableau typé quand le moteur le permet, sinon en base64, choix
   mesuré par adaptateur. Aucun format JSON ni objet natif propre au moteur n'est introduit.

### Une interface de transport sous `WebLink` et dans `WebFront.js`

Côté C++, `WebLink` ne dépendra que d'une interface de transport (envoyer des octets, recevoir un message, fermer,
signaler une déconnexion), dont le WebSocket actuel devient une implémentation. Côté JavaScript, `WebFront.js`
choisira le transport direct lorsqu'un objet de canal, injecté par l'adaptateur au début du document et avant tout
script de la page, est présent ; sinon le WebSocket. Le handshake et `webFront.ready` gardent leur sémantique.

Les contrats ne changent pas avec le transport : Promise systématique côté JavaScript, appel non typé sans retour et
`std::future`/sender typés côté C++, échec en cas de déconnexion ([ADR-004](ADR-004-appels-asynchrones-et-erreurs.md)) ;
tâches possédées, annulées et jointes ([ADR-006](ADR-006-taches-structurees-et-arret.md)) ; cycle de vie et action
de fin de session de l'ADR-007.

### Fils d'exécution

Les messages directs arrivent sur le thread du toolkit, en général le thread principal. Le transport les remet au
contexte d'exécution du serveur avant tout décodage applicatif ; les callbacks applicatifs ne s'exécutent jamais sur
le thread de l'interface sans décision explicite. Inversement, les envois C++ → JavaScript repassent par le thread
du toolkit lorsque le moteur l'exige (WebKitGTK, WKWebView, WebView2). Les règles de complétion de
[ASYNC_EXECUTION](../ASYNC_EXECUTION.md) restent celles du chemin HTML.

### Origines

Un canal direct n'est accepté que du document principal de l'origine de l'application, avec le filtrage natif du
moteur (règles d'origine d'Android, source de WebView2, cadre principal de WebKit) ; une origine refusée échoue comme
une connexion WebSocket refusée. Comme l'ADR-008, cette restriction n'est pas un bac à sable.

### Mobile

Les futurs frontends Android et iOS seront d'abord directs (ressources et messages) ; le serveur HTTP y reste
disponible sur option, pour le débogage par un navigateur ou l'accès distant. Ils satisfont `FrontendType` et
héritent des scénarios de cycle de vie partagés.

### L'accès direct par défaut des frontends natifs ?

Question posée : puisqu'un frontend natif n'a pas besoin d'un serveur, l'accès direct ne devrait-il pas être son défaut,
ce qui éviterait les autorisations réseau ? Évaluation, d'après les mesures ci-dessus et la documentation :

- **Autorisations évitées.** Un socket d'écoute, même sur la boucle locale, demande `android.permission.INTERNET` à une
  application Android. Les éventuelles exceptions de HTTP en clair et d'ATS dépendent des versions et de la
  configuration décrites plus haut ; leur suppression ne constitue pas à elle seule une justification universelle du
  défaut direct. L'application peut en outre être suspendue en arrière-plan. Sur poste de travail,
  un écouteur lié à `127.0.0.1` ne déclenche en pratique pas l'invite du pare-feu Windows (qui concerne les écouteurs
  ouverts à toutes les interfaces), mais un antivirus, une stratégie d'entreprise ou un autre processus de l'utilisateur
  peuvent encore le voir ; le port par défaut `9002` entre aussi en conflit entre deux applications. Ces points sont à
  vérifier sur chaque plateforme cible et non démontrés par ce benchmark.
- **Performance.** Elle ne tranche pas : le canal de chaînes de WebView2 est plus lent que le HTML, celui de WebKitGTK plus
  rapide. Elle ne justifie un défaut direct que si le transport est conçu autour des canaux binaires.
- **Outils.** L'environnement de développement (terminal, chargement des modules, navigateur système) a besoin du
  serveur HTTP ; le chemin HTML ne peut donc pas disparaître d'un projet de bureau, et un défaut direct ne vaut que pour
  la version distribuée.

Recommandation, sans modifier l'état « Proposée » :

| Cible | Défaut recommandé | Condition |
| --- | --- | --- |
| Android, iOS | direct | Cycle de vie et exposition du port ; règles réseau à qualifier selon les versions prises en charge, sans présumer d'une exception obligatoire partout. |
| WebView2, WKWebView, WebKitGTK, CEF (distribution embarquée) | HTML jusqu'à preuve, puis direct | Critères 1 à 3 ci-dessous remplis, mesures codec et changement de thread comprises, sans régression de la suite Jasmine ; sous WebView2 seulement avec le tampon partagé. |
| Navigateur système, clients distants, micro-contrôleurs | HTML | Seul chemin existant. |

Dans tous les cas, le chemin HTML reste disponible par une option explicite, et le choix du défaut est une décision
d'adaptateur qui ne change ni l'API applicative ni le format.

## Critères d'acceptation

Chacun fera l'objet d'une issue dédiée :

1. L'interface de transport existe dans `WebLink` et `WebFront.js` ; le WebSocket en est une implémentation et toutes
   les suites existantes passent sans modification de leurs attentes.
2. Un premier transport direct (WebKitGTK ou WKWebView) passe la suite Jasmine partagée par le canal direct, et le
   benchmark mesure ce transport complet (codec et changement de thread compris) face au chemin HTML.
3. Les mesures CEF sont ajoutées à cette ADR avec leur configuration (WebKitGTK, WebView2 et WKWebView sont mesurés).
   Sous WKWebView, le coût base64 et les règles du schéma applicatif sont vérifiés sur macOS et iOS ; le défaut de chaque adaptateur est arrêté d'après le transport complet.
4. Un prototype Android ou iOS charge ses ressources et échange ses messages sans serveur local.

## Options écartées et motifs

- **Remplacer le HTML par l'accès direct.** Perd le navigateur système, les clients distants et les micro-contrôleurs,
  et ferait dépendre le cœur de chaque moteur.
- **Garder le serveur local comme seule voie sur mobile.** Peut exiger des exceptions réseau selon les versions, subit la
  suspension en arrière-plan et expose un port aux autres applications de l'appareil.
- **Un format propre au canal direct (JSON, objets natifs).** Crée un second codec par moteur, diverge de l'ADR-003
  et perd les tableaux typés et les bitsets compacts ; les mesures ne montrent aucun besoin qui le justifie.
- **Exposer à l'application les API natives des moteurs.** Rompt la portabilité du code applicatif et le contrat
  `FrontendType` ; l'accès direct reste interne à l'adaptateur.
- **Un canal synchrone (interfaces JavaScript bloquantes).** Contredit l'asynchronisme de l'ADR-004 et bloquerait le
  thread de rendu.

## Conséquences

- **Bénéfice :** des frontends mobiles sans serveur local, un canal non exposé aux autres processus et un débit
  concurrent de 2,0 à 2,3 fois supérieur (mesuré sur WebKitGTK), sans changer le code applicatif ni le protocole.
- **Bénéfice immédiat :** le chemin HTML, qui reste celui de tous, est plus rapide (mesure historique du 2026-10-24, 32 Kio : de 2380 à 705 µs).
- **Coût accepté :** deux transports à tester pour chaque moteur direct ; la suite Jasmine et les scénarios de cycle
  de vie devront s'exécuter sur chacun.
- **Risque :** divergence de comportement entre transports (ordre des messages, déconnexion, taille maximale des
  messages du moteur). Les contrats sont exprimés au niveau de l'interface de transport et prouvés par les mêmes
  suites sur les deux chemins.

## Prescriptions d'implémentation et de génération de code

- Garder le chemin HTML disponible et testé sur chaque plateforme ; permettre de désactiver son serveur au démarrage
  lorsqu'un transport direct est choisi. Ne pas faire dépendre le cœur d'un moteur ou d'un transport direct.
- Ne pas introduire de codec, de type filaire ni de sémantique d'appel propre à un transport ; toute évolution du
  format suit l'ADR-003 sur tous les transports à la fois.
- Remettre les messages directs au contexte d'exécution du serveur ; ne pas exécuter de callback applicatif sur le
  thread du toolkit.
- Filtrer le canal direct par origine et cadre principal ; ne pas présenter ce filtrage comme un bac à sable.
- Mesurer avec `WebKitGTKTransportBenchmark` (ou son équivalent pour le moteur) avant et après tout changement du
  chemin chaud ; un benchmark échoue sur une réponse incorrecte, jamais sur un seuil de temps.
- Ne pas formater de journal coûteux sans tester d'abord son niveau (`log::is`) sur le chemin des messages.

## Sources et vérification

- Règles réseau mobiles : [Android, configuration de localhost](https://developer.android.com/privacy-and-security/security-config#localhost),
  [Apple, NSAllowsLocalNetworking](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)
  et [exceptions ATS par adresse IP](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsexceptiondomains)
  (consultés le 2026-10-25). Les permissions d'écoute, ATS et la politique du trafic en clair sont des règles distinctes.

- Benchmarks manuels : [WebKitGTKTransportBenchmark.cpp](../../test/web/WebKitGTKTransportBenchmark.cpp),
  [WebView2TransportBenchmark.cpp](../../test/web/WebView2TransportBenchmark.cpp),
  [transport-benchmark.html](../../test/web/transport-benchmark.html). Construire explicitement
  `webkitgtk_transport_benchmark` ou `webview2_transport_benchmark`, puis lancer l'exécutable
  depuis `test/web` avec le chemin du rapport JSON en argument. WebKitGTK demande Xvfb sans
  affichage ; WebView2 demande Windows et Evergreen. Ces outils utilisent Release sans
  couverture pour les mesures. Les cas chronométrés CTest et la passe CI `performance-linux`
  ont été retirés par #283 ; [FRONTENDS](../FRONTENDS.md) donne les commandes manuelles.
  Les tests courts `WebKitGTKTransportContracts` et `WebView2TransportContracts` conservent
  les vérifications des réponses natives et des erreurs via `transport-contracts.html`, sans
  lots de mesure ni rapport de performance ; WebView2 conserve aussi les appels par buffer partagé.
  La partie commune est dans [TransportBenchmark.hpp](../../test/web/TransportBenchmark.hpp).
- Régressions du chemin chaud : `Encoding bridge messages writes nothing to the standard output`
  ([MessagesTests](../../test/unit/MessagesTests.cpp)) et `FrameBufferNotFormattedWhenDebugDisabled`
  ([FrameAllocationTest](../../test/unit/FrameAllocationTest.cpp)).
- Benchmark WKWebView : [WKWebViewTransportBenchmark.cpp](../../test/web/WKWebViewTransportBenchmark.cpp),
  [sondes d'API](../../test/web/wkwebview-transport-contracts.html), enregistrement dans
  [WKWebViewSupport.cmake](../../cmake/WKWebViewSupport.cmake). Configuration macOS :
  `-DWEBFRONT_EMBED_WKWEBVIEW=ON -DWEBFRONT_WEBTEST_FRONTEND=WKWebView -DWEBFRONT_ENABLE_TESTING=ON` ;
  `ctest --test-dir <build> -R '^WKWebViewTransport(Benchmark|Contracts)$' --output-on-failure`.
- API publiques Apple (documentation et en-têtes du SDK macOS, vérifiés le 2026-10-25) :
  [corps des messages](https://developer.apple.com/documentation/webkit/wkscriptmessage/body),
  [réponses natives](https://developer.apple.com/documentation/webkit/wkscriptmessagehandlerwithreply/usercontentcontroller(_:didreceive:replyhandler:)),
  [arguments et Promises de callAsyncJavaScript](https://developer.apple.com/documentation/webkit/wkwebview/callasyncjavascript(_:arguments:in:in:completionhandler:)),
  [exposition aux cadres](https://developer.apple.com/documentation/webkit/wkusercontentcontroller/addscriptmessagehandler(_:contentworld:name:)),
  [frameInfo et securityOrigin](https://developer.apple.com/documentation/webkit/wkframeinfo),
  [schémas personnalisés](https://developer.apple.com/documentation/webkit/wkwebviewconfiguration/seturlschemehandler(_:forurlscheme:)),
  [arrêt d'une tâche de ressource](https://developer.apple.com/documentation/webkit/wkurlschemehandler/webview(_:stop:)).
- Autorisations réseau : [règles Android, dont localhost à l'API 37](https://developer.android.com/privacy-and-security/security-config#localhost-configuration),
  [ATS et versions d'iOS/macOS](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking).
- Voir [FRONTENDS](../FRONTENDS.md), l'ADR-003, l'ADR-004, l'ADR-007 et l'ADR-008.
