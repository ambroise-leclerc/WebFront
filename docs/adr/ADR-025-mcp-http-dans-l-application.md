# ADR-025 : Servir le MCP en Streamable HTTP depuis l'application, sur la boucle locale

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-21 | Proposée | Ambroise Leclerc | Issue #632, après #617 (instances simultanées) et #631 (TLS 1.3 intégré) : l'application sert elle-même le MCP en Streamable HTTP sur un écouteur loopback distinct, authentifié par un jeton Bearer de session ; accès distant exclu ; Host et Origin stricts ; découverte par instance ; automatisation refusée à la compilation d'une build de distribution ; ordre de migration et sort de `webfront-agent`. Amende l'ADR-019 (ligne « MCP Streamable HTTP local » du comparatif, section « CLI et MCP local stdio » et ligne « DNS rebinding » du modèle de menace). | — |
| 1.1 | 2026-11-22 | Acceptée (livraison à faire) | Ambroise Leclerc | Décision acceptée avant le code de transport, comme l'exige #632. Précision d'implémentation du point 2 : l'écouteur reprend le modèle d'admission du canal actuel (file, délais, workers) et l'analyseur `http::Request` de `HTTPServer.hpp`, plutôt qu'une instance de `http::Server`, qui n'a ni file bornée ni délai de réception. | Issue #632. |
| 1.2 | 2026-11-22 | Acceptée (livraison à faire) | Ambroise Leclerc | Étape 4, arbitrée par le propriétaire : le but de #632 est un seul exécutable, l'application de l'utilisateur. `webfront-agent` est retiré en entier (relais stdio, CLI JSON, `--serve`, `--attach`, `--list`, `--print-mcp-config`, `webfront_add_agent()`, `WEBFRONT_BUILD_AUTOMATION`), sans exemple `agent-host` ; le canal JSON-lines disparaît avec lui. HTTP devenant le seul transport, l'option `.http` est supprimée et `listen` devient `directory`, facultatif. Les régressions pilotent un programme de test (`automation_host`) qui appelle `enableAutomation`. Les statuts 70 et 71 du lanceur disparaissent avec lui ; le 72 d'`onUnresponsive` reste. | Issue #632. |

## État et portée

**Acceptée le 2026-11-22**, avant tout code de transport (critère de #632) ; la livraison suit l'ordre de « Migration ». Elle remplace,
une fois livrée, les parties « CLI et MCP local stdio » et « Attachement à la même fenêtre » de l'[ADR-019](ADR-019-developpement-automatisable-et-agents.md) ;
le contrat v1 (`Contract.hpp`), le contrôleur (`Session.hpp`), les grants natifs, la liste blanche de `call`, la liaison de page
(`Binding.hpp`, secret de lancement, preuves HMAC) et le modèle de menace de l'ADR-019 restent en vigueur. Elle ne modifie pas
le codec binaire (ADR-003), ni le contrôle d'origine du pont (ADR-008), ni le fournisseur TLS (ADR-024).

## Contexte

Le MCP passe aujourd'hui par un second exécutable, `webfront-agent`, qui relaie un canal JSON-lines privé (TCP 127.0.0.1,
capacité de 256 bits dans un répertoire privé) vers un MCP sur stdio. Une personne qui découvre WebFront doit comprendre
deux processus, un canal intermédiaire et deux modes (`--serve` et `enableAutomation`), activer `WEBFRONT_BUILD_AUTOMATION`,
appeler `webfront_add_agent()` puis lancer `webfront-agent --attach DIR --mcp` ou générer sa configuration.

Trois livraisons changent les données :

- **#619** : le serveur HTTP accepte `POST` sur une route qui l'autorise (`RouteOptions::postBodyLimit`), avec un corps borné
  annoncé par `Content-Length`, `Expect: 100-continue`, et les refus 411/413/501 (pas de corps chunked).
- **#617** : plusieurs instances tournent côte à côte. Un port demandé à `0` est attribué par l'OS et connu de l'application
  (`Server::port()`), un port choisi n'est jamais remplacé et l'échec de liaison nomme l'adresse, le port et la cause. Il n'y a
  donc plus d'URL connue d'avance : la découverte doit désigner une instance.
- **#631** : le TLS 1.3 intégré rend possible un `https://` sans mandataire, donc un agent distant chiffré. TLS protège le
  transport et authentifie le serveur, pas les clients (ADR-008, ADR-024).

Un serveur HTTP loopback est joignable par les pages web de la machine, et l'application embarque justement un navigateur.
L'ADR-019 classait ce transport « différé jusqu'à preuve d'un besoin » et la menace « DNS rebinding » comme critique s'il
était retenu. Le besoin est la **simplicité de compréhension** et la découverte par les agents, pas la réduction du code :
le chiffrage de #632 estime 750 à 900 lignes retirées pour 400 à 500 ajoutées.

## Possibilités comparées

| Voie | Intérêt | Limites | Position |
| --- | --- | --- | --- |
| Statu quo : `webfront-agent` relais stdio | Le client lance le processus ; aucun service réseau MCP. | Deux processus, un canal privé, deux modes ; configuration par chemin d'exécutable. | Conservé pendant la migration, puis retiré (voir « Migration »). |
| Route `/mcp` sur le serveur qui sert la page | Un seul port. | Une page de même origine atteint la route ; le contrôle d'origine du pont et le cookie de liaison partagent ce port ; un écouteur TLS ou non loopback l'exposerait. | Écartée. |
| **Écouteur MCP distinct, loopback, jeton Bearer** | Une URL par instance, aucun second exécutable, client MCP standard ; la page n'est jamais sur cette origine. | Service réseau privilégié ; le client ne lance plus l'application ; jeton à transmettre hors du projet. | **Retenue.** |
| MCP HTTPS distant (#631) avec jeton | Agent en conteneur, WSL ou autre machine. | Identité des clients, modèle multi-utilisateur, rotation et révocation, exposition réseau, OAuth attendu par les clients MCP distants. | Hors périmètre ; décision dédiée. |
| MCP sur socket Unix / tube nommé | Pas de port, permissions du système de fichiers. | Peu de clients MCP le prennent en charge ; pas de transport MCP normalisé. | Écartée. |

## Décision

### 1. Périmètre : boucle locale seulement

L'écouteur MCP se lie à `127.0.0.1` et à rien d'autre. Il n'utilise jamais le fournisseur TLS, l'adresse ni le port de
`WebFrontConfig` : une application servie en HTTPS sur une adresse publique garde un MCP loopback en `http://`. `localhost`
est un contexte local et le transport est **déclaré** loopback, conformément à la règle « transport déclaré, jamais déduit »
de l'ADR-020 ; l'écouteur ne se marque jamais sécurisé. L'accès distant, en HTTPS ou non, reste hors périmètre : il exige une
authentification des clients et un modèle multi-utilisateur que ni l'ADR-008 ni l'ADR-024 ne fournissent. Un agent en
conteneur ou en machine virtuelle utilise un transfert de port qu'il établit lui-même ; WebFront ne le promet pas.

### 2. Écouteur séparé de celui de la page

Le MCP a son propre écouteur, sur un port distinct de celui de l'application. Il ne sert qu'un chemin, `/mcp` ; tout autre
chemin, y compris `/WebFront.js`, les fichiers, les routes de l'application et les découvertes OAuth (`/.well-known/...`),
reçoit 404. Il n'accepte aucune mise à niveau WebSocket. Le serveur qui sert la page n'a jamais de route `/mcp`. La page de
l'application et une page étrangère sont donc sur une autre origine que le MCP, sans jeton, et soumises aux refus du point 4.

L'implémentation réutilise l'analyseur `http::Request` et les aides de `HTTPServer.hpp` (longueur annoncée, `Expect:
100-continue`, refus 411/413/501) dans le modèle d'admission du canal actuel (point 7), et non une instance de
`http::Server` : celle-ci sert fichiers et ressources embarquées, et n'a ni file bornée de pairs ni délai de réception.
L'écouteur MCP n'a ni fournisseur de fichiers, ni ressource embarquée, ni gestionnaire d'upgrade.
Le port est celui que l'OS attribue (`0`), selon le mécanisme de #617 ; une option permet un port choisi, avec les règles de
#617 (jamais remplacé, échec qui nomme l'adresse, le port et la cause). Aucune politique de port propre au MCP n'est introduite.

### 3. Authentification : jeton Bearer de session

Chaque activation tire un jeton de 256 bits de l'aléa de l'OS, distinct de la capacité du canal JSON-lines, du secret de
lancement de la page et de l'identifiant de session. Chaque requête porte `Authorization: Bearer <jeton>` ; la comparaison
est à temps constant et précède la lecture du JSON. Sinon : 401 avec `WWW-Authenticate: Bearer`, sans `resource_metadata`,
car aucun serveur d'autorisation OAuth n'est annoncé. Le jeton vit autant que la session : il est révoqué à sa fermeture
(fermeture normale, signal interceptable, échéance native, fenêtre perdue, watchdog), l'écouteur est alors fermé, et une
nouvelle activation tire un nouveau jeton. Pas de rotation pendant la session.

Le jeton n'apparaît jamais dans la page, une URL, une ligne de commande, les journaux, le message affiché, l'enregistrement
de découverte ni un fichier du projet, que le pont de fichiers du développeur peut lire. Il est écrit dans le répertoire
privé de l'instance (point 5), dans un fichier de configuration cliente `mcp.json` en mode 0600 (ACL du propriétaire sous
Windows), qui contient l'URL et l'en-tête :

```json
{"mcpServers": {"<nom>": {"type": "http", "url": "http://127.0.0.1:PORT/mcp",
                          "headers": {"Authorization": "Bearer <jeton>"}}}}
```

Le client l'obtient par ce fichier hors projet : en le passant tel quel à un client qui accepte un fichier de configuration,
en recopiant l'en-tête dans la configuration **utilisateur** du client (hors projet), ou par un `headersHelper` qui lit le
fichier. Une variable d'environnement n'est pas retenue : le jeton change à chaque lancement et l'environnement du client
est fixé avant. Le `.mcp.json` d'un projet ne contient jamais le jeton ; la documentation le dit et `wf` n'en écrit pas.

### 4. Attaques par navigateur

Avant l'authentification, et donc même avec un jeton valide :

- **Host** doit valoir exactement `127.0.0.1:<port>` ou `localhost:<port>` ; sinon 421. Cela écarte le rebinding DNS.
- **Origin** non vide : 403, quelle que soit sa valeur, y compris l'origine de l'application. Un client MCP natif n'envoie
  pas d'`Origin` ; un navigateur l'envoie sur tout `POST`. `Sec-Fetch-Site` ou `Sec-Fetch-Mode` présent : 403 aussi.
- **Content-Type** doit être `application/json` : une requête « simple » de formulaire est refusée (415), et la requête de
  pré-vérification `OPTIONS` reçoit 405 sans aucun en-tête CORS. Aucune réponse ne porte `Access-Control-Allow-*`.

Ces refus ne remplacent pas le jeton : un client natif peut forger `Host` et omettre `Origin` (ADR-008). Le jeton seul
authentifie ; Host, Origin et Content-Type empêchent qu'une page web l'utilise ou sonde la route.

### 5. Adresse, découverte et nettoyage

`enableAutomation` n'exige pas de répertoire : il crée un répertoire privé neuf par instance sous le répertoire de
découverte par utilisateur existant (`.../webfront/sessions/<session>/`, 0700), nommé par l'identifiant de session (aléa
de l'OS), donc sans collision entre des centaines d'instances. Un `directory` explicite reste accepté (neuf, hors projet). L'enregistrement `<session>.json` gagne l'URL MCP (sans jeton)
et le chemin de `mcp.json`. L'application affiche une ligne sur stderr (désactivable par `announce = false`) :

```text
WebFront MCP (grants: check, build): http://127.0.0.1:PORT/mcp — client configuration: <répertoire>/mcp.json
```

`Automation::mcpUrl()` et `Automation::clientConfiguration()` rendent la même information au code. À la fermeture, le
répertoire, ses fichiers connus (`mcp.json`) et l'enregistrement sont retirés, sans suppression récursive
d'un contenu inconnu. Après un arrêt non interceptable, l'activation suivante et la liste des sessions retirent les
enregistrements dont le processus n'existe plus, avec leurs fichiers connus. Un identifiant de processus recyclé peut
retarder ce nettoyage ; le jeton d'une instance morte ne sert à rien, son port étant fermé.

### 6. Protocole

Profil Streamable HTTP de MCP **2025-11-25**, réduit :

- Un seul point d'accès `POST /mcp`, un message JSON-RPC par requête ; un tableau (lot) est refusé (`-32600`). Réponse
  toujours `application/json`, jamais SSE ; `Accept` doit contenir `application/json`. Une notification ou une réponse
  du client reçoit 202 sans corps. `GET /mcp` reçoit 405 (pas de flux serveur).
- Corps borné par `Content-Length` à 60 000 octets (la limite d'entrée actuelle), profondeur JSON 32, clés dupliquées
  refusées ; pas de corps chunked (501), `Expect: 100-continue` honoré, limites du serveur actuel.
- `Mcp-Session-Id` est attribué à `initialize` (128 bits de l'OS) ; il route, il n'authentifie pas. Une requête sans lui
  hors `initialize` reçoit 400, un identifiant inconnu ou terminé 404 ; `DELETE /mcp` termine la session MCP, pas la
  session d'automatisation ni la fenêtre. Seize sessions MCP au plus ; la plus anciennement inactive est évincée.
- `MCP-Protocol-Version` est vérifié après l'initialisation ; une version non prise en charge reçoit 400.
- Le catalogue reste à quatre outils : `webfront.session`, `webfront.start`, `webfront.result`, `webfront.cancel`. Une
  opération longue se suit par interrogation de `webfront.result` ; aucune notification de progression n'est poussée.
- La réponse à `initialize` porte des `instructions` courtes (celles de `Instructions.hpp`). Les ressources MCP
  (`resources/list`) relèvent d'une évolution indépendante du transport.

### 7. Garanties reprises du canal actuel

- File de 64 pairs non authentifiés, éviction du plus ancien, délai de deux secondes pour recevoir en-têtes et corps ;
  32 workers pour les requêtes authentifiées, qu'un pair silencieux ne consomme pas. Une saturation locale peut encore
  épuiser l'OS ou le processeur.
- Délai par requête de 30 secondes côté serveur ; les échéances par opération (`timeout`) et de démarrage
  (`startupTimeout`) sont inchangées ; la progression ne les prolonge pas.
- Sockets non héritables, fermées à l'exécution sous POSIX.
- Révocation : fermeture de la session, de l'écouteur et du jeton ; watchdog, `closeGrace`, `onUnresponsive` et le statut
  72 de l'application inchangés. Les statuts 70 et 71 restent ceux du lanceur tant qu'il existe.
- Audit des décisions d'autorisation et rétention de 64 runs inchangés ; un refus d'authentification HTTP n'atteint pas le
  contrôleur et n'évince pas l'audit.

### 8. Garde de compilation

L'automatisation n'existe pas dans une build de distribution : `Enable.hpp`, et tout en-tête qui ouvre un écouteur MCP,
produit une erreur de compilation si `WEBFRONT_DISTRIBUTION` vaut 1. C'est un durcissement : aujourd'hui, la documentation
laisse ce choix à l'application (« gate the call »). Une application qui le faisait déjà n'est pas affectée ; une
application qui l'oubliait ne compile plus en distribution, au lieu d'expédier un écouteur.

### 9. Démarrage par l'agent

Avec stdio, le client lance le processus ; avec HTTP, l'application doit déjà tourner. Ce n'est pas compensé par un
lanceur : l'application appartient à la personne qui la développe, qui la lance comme d'habitude. Un agent disposant d'un
shell peut la lancer en arrière-plan puis attendre l'apparition de `mcp.json` ou de l'enregistrement de session ;
`DEVELOPING_WITH_AGENTS.md` décrit cette attente. Un client qui ne peut que lancer des processus stdio n'est plus pris en
charge après la migration.

### 10. Sort de `webfront-agent`, de son CLI et de `--serve` (version 1.2)

Le but de #632 est un seul exécutable : l'application de l'utilisateur est le serveur MCP. `webfront-agent` est retiré en
entier, sans être transféré dans les exemples :

- le relais MCP stdio (`--mcp`, `--attach --mcp`), `--print-mcp-config`, `--list`, `--serve`, le CLI JSON (`--operation`,
  `--schema`, `--agent-help`), `webfront_add_agent()`, `WEBFRONT_BUILD_AUTOMATION` et `WEBFRONT_AUTOMATION_FRONTEND` ;
  définir l'une de ces options n'a plus d'effet qu'un avertissement de configuration ;
- le canal JSON-lines (`ChannelServer`, `ChannelClient`) et le lecteur stdio ; `Channel.hpp` ne garde que les sockets et les
  fichiers privés de l'écouteur ;
- l'option `.http`, puisque HTTP est le seul transport ; `listen` devient `directory`, facultatif ;
- les statuts 70 (amorçage) et 71 (échéance) du lanceur ; `onUnresponsive` garde son statut 72 par défaut.

Les régressions navigateur pilotent `automation_host`, un programme de test (non livré) qui appelle `enableAutomation` avec
le provisionnement du pilote de test ; les clients Python (`AutomationClient.py`, `AutomationSdkClient.py`,
`AutomationModelBenchmark.py`) parlent HTTP. Le profil temporaire et le nettoyage des processus du navigateur propres au
lanceur disparaissent : une application garde son profil habituel (#617).

## Modèle de menace : ce qui change

Les menaces, actifs et risques résiduels de l'ADR-019 restent valables. Ce qui change :

| Menace et gravité | Mesures | Risque résiduel |
| --- | --- | --- |
| Rebinding DNS, page web qui vise le port MCP — critique | Écouteur loopback distinct ; Host exact ; tout `Origin` et tout `Sec-Fetch-*` refusés ; `application/json` exigé ; aucun CORS ; jeton exigé en plus. | Un navigateur qui omettrait `Origin` sur un `POST` cross-origin et ignorerait la pré-vérification resterait sans jeton. |
| Client local non autorisé — critique | Jeton de 256 bits, comparaison à temps constant, avant tout traitement ; fichier privé hors projet. | Un processus du même utilisateur peut lire `mcp.json`, comme il lisait le fichier du canal JSON-lines. |
| Fuite du jeton — élevée | Jamais dans le projet, la page, l'URL, la ligne de commande, les journaux, l'affichage ou l'enregistrement ; vie limitée à la session. | Une personne qui copie le fichier dans le projet ou un dépôt l'expose ; la documentation l'interdit, aucun contrôle ne l'empêche. |
| Exposition réseau — critique | Liaison 127.0.0.1 seule, indépendante de l'adresse et du TLS de l'application ; adresse non configurable. | Un transfert de port établi par l'utilisateur étend l'accès hors de cette décision. |
| Automatisation expédiée — critique | Erreur de compilation sous `WEBFRONT_DISTRIBUTION`. | Une build qui ne définit pas cette macro n'est pas protégée par elle. |
| Épuisement de connexions — élevée | File bornée, délais, workers séparés (point 7) ; sessions MCP bornées. | Une saturation locale de l'OS reste possible. |

## Migration

1. **ADR** : cette décision, relue et acceptée avant tout code de transport.
2. **Transport HTTP** dans l'application, à côté de `webfront-agent`, avec les tests unitaires de sécurité (jeton absent,
   faux, révoqué ; Host étranger ; `Origin` hostile et de même application ; `Sec-Fetch-*` ; `Content-Type` ; lot ;
   `Expect: 100-continue` ; corps trop grand ou chunked ; session MCP absente, inconnue, terminée ; garde de
   distribution) et la reprise des scénarios `AutomationMcpWindow`, `AutomationMcpWatchdog`, `AutomationConsumerWindow`
   et `AutomationSdkWindow` sur HTTP, plus un scénario à deux instances simultanées.
3. **Documentation centrée sur l'URL** : `AUTOMATION.md`, `DEVELOPING_WITH_AGENTS.md`, README, `AGENTS.md` des starters,
   `webfront_write_agent_guide()`, `PROJECT_STATUS.md` et `CHANGELOG.md`.
4. **Retrait** de `webfront-agent` en entier et du canal JSON-lines (point 10, version 1.2).

## Critères d'acceptation de la livraison

- Une application qui fait `enableAutomation(app, {.grants = {...}})` répond au MCP ; un client MCP standard (SDK Python
  officiel) fait `initialize`, `tools/list` et une opération complète (`check`, `build`), sans autre programme.
- Deux applications simultanées exposent chacune leur MCP sans collision, et l'agent désigne l'instance voulue par son URL
  et son `mcp.json`.
- Les cas de sécurité de l'étape 2 sont couverts par des tests, et l'automatisation ne compile pas en distribution.
- Les quatre scénarios cités passent sur le nouveau transport.
- Les guides, le README et `PROJECT_STATUS.md` décrivent le nouveau parcours.

## Conséquences

- **Bénéfice :** une URL par instance, un client MCP standard, plus de second exécutable ni de canal intermédiaire pour
  le parcours principal ; découverte par l'affichage, `mcp.json` et l'enregistrement de session.
- **Coût accepté :** un service réseau privilégié dans l'application de développement, un jeton à transmettre au client,
  l'application qui doit tourner avant que l'agent se connecte, et la perte des clients limités à stdio.
- **Risque :** que l'accès distant soit demandé ensuite ; il exige une décision dédiée sur l'authentification des
  clients, et ne doit pas être obtenu en liant l'écouteur ailleurs que sur la boucle locale.

## Références

- [ADR-019](ADR-019-developpement-automatisable-et-agents.md), [ADR-008](ADR-008-contenu-de-confiance-et-origines.md),
  [ADR-020](ADR-020-services-systeme.md) (transport déclaré), [ADR-024](ADR-024-tls-integre.md),
  [AUTOMATION](../AUTOMATION.md), [HOST_INTEGRATION](../HOST_INTEGRATION.md) (port et instances, #617).
- [MCP 2025-11-25, transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports) (Streamable HTTP,
  `Mcp-Session-Id`, `MCP-Protocol-Version`, validation d'`Origin`).
- [MCP, bonnes pratiques de sécurité](https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices).
