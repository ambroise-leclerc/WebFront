# ADR-019 : Rendre l'environnement développeur automatisable par des agents

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-28 | Proposée | Ambroise Leclerc | Comparaison des voies d'automatisation, orientation MCP locale et modèle de menace de l'épique #403 ; #402 comme premier cas consommateur. | À renseigner après intégration. |
| 1.1 | 2026-10-28 | Proposée | Ambroise Leclerc | Contrat d'opérations décrit en JSON Schema et projeté vers plusieurs adaptateurs ; CLI et consignes d'agent comme voie de premier rang ; WebDriver BiDi et WebMCP au comparatif ; exigences propres aux modèles locaux ; précisions sur le premier incrément et la gravité conditionnelle de HTTP. | — |
| 1.2 | 2026-10-29 | Acceptée | Ambroise Leclerc | Décision fondée sur les livraisons #405, #406 et #407 : contrat v1, CLI/MCP stdio, attachement authentifié, protections et réserves ; qualification de cinq modèles locaux, preuves CI et préparation de la distribution 0.6.3. | [#407](https://github.com/ambroise-leclerc/WebFront/pull/407), tête `4a1c92b`, intégration `22f6795` ; révision ADR `cf1bf15`. |

## État et portée

**Acceptée le 2026-10-29**, pour le premier incrément de l'[épique #403](https://github.com/ambroise-leclerc/WebFront/issues/403).
La comparaison a été intégrée par [#405](https://github.com/ambroise-leclerc/WebFront/pull/405),
Jasmine/CTest consommateur par [#406](https://github.com/ambroise-leclerc/WebFront/pull/406),
et l'automatisation par [#407](https://github.com/ambroise-leclerc/WebFront/pull/407), fusionnée dans develop.
L'acceptation porte sur une architecture locale pour projets de confiance, pas sur une sandbox,
une compatibilité universelle MCP ou une garantie générale de réussite des modèles.

La distribution développeur 0.6.3 est celle préparée avec ces sources. La publication et la bascule
du pin suivent [PUBLISHING](../../js/developer/tools/PUBLISHING.md) : un pin vers une archive nouvelle
ne doit pas être fusionné avant vérification du déploiement. Le binaire natif reste une cible CMake
optionnelle ; aucun installateur natif multiplateforme n'est promis par cette décision.

## Contexte et contraintes

L'[issue #402](https://github.com/ambroise-leclerc/WebFront/issues/402) demande de lancer le vrai Jasmine
dans la page d'une application externe depuis CTest, avec ses arguments et son documentRoot.
Cette voie est indépendante de MCP et reste utilisable sans environnement développeur dans l'application.
Le terminal wf et le pilote interne webfront-wf fournissaient déjà un dispatch non interactif ;
le contrat d'automatisation réutilise ce moteur au lieu de lire le rendu d'un terminal.

Le cœur reste C++23 header-only. Les outils sont optionnels, hors des ressources minimales de
l'ADR-016. Le poste n'a besoin ni de Node/npm ni de Python pour exécuter le lanceur ; Python et
un SDK sont utilisés par des tests optionnels. Bootstrap et paquets sont épinglés et vérifiés.
Les tests configurés ne requièrent pas de réseau public. Le bridge demeure asynchrone et borné
(ADR-003/004), et l'arrêt appartient au natif (ADR-006).

Le code de projet, les scripts, les tests et les callbacks conservent leurs droits sur l'hôte.
L'intégrité des archives n'établit pas leur innocuité. Le contrôle d'origine et les permissions de
manifeste ne sont pas une isolation OS (ADR-008). CDP n'est pas un prérequis commun aux moteurs.

## Possibilités comparées

Les coûts ci-dessous sont des estimations qualitatives de dépendances, d'attaque et de maintenance.
Les mesures effectuées sur l'implémentation sont distinguées dans le bilan de validation.

| Voie | Intérêt | Limites et coût | Position retenue |
| --- | --- | --- | --- |
| CLI structurée issue du pilote interne | Simple pour CI, CTest et scripts ; réutilise le provisioning et les watchdogs. | Publiciser packaging, schémas et cycle de vie ; la CLI seule décrit mal une fenêtre partagée et ses événements. Coût moyen. | Retenue : CLI JSON et contrat v1, utilisables sans MCP. |
| Consignes d'agent (fichier de consignes ou skill) sur la CLI | Les agents disposant d'un shell la découvrent à la demande, sans protocole ni catalogue d'outils chargé à chaque tour ; fonctionne avec la plupart des modèles, y compris locaux. | Ni découverte typée ni négociation de capacités ; l'autorité reste celle du shell qui la lance. Coût faible au-dessus de la CLI. | Retenue : consignes accessibles par --agent-help. |
| MCP local sur stdio | Découverte des outils et schémas par les clients compatibles ; processus lancé par le client, sans endpoint MCP réseau. | Adaptation du protocole, packaging, stdout exclusivement protocolaire, compatibilité clients ; accès au processus toujours sensible. Coût moyen au-dessus du socle. | Retenue : adaptateur natif limité à stdio 2025-11-25. |
| MCP Streamable HTTP local | Service persistant, plusieurs clients, attachement naturel à une fenêtre existante. | Authentification, Host/Origin, révocation, connexions et sessions ; nouveau service réseau privilégié. Coût élevé. | Différé jusqu'à preuve d'un besoin non couvert par stdio et IPC natif. |
| Pilotage navigateur : accessibilité/DOM, captures, Playwright ou CDP | Vérifie l'IHM visible et les interactions ; outils génériques utiles aux agents. | Sélecteurs fragiles, captures coûteuses et sensibles, fonctions différentes selon moteur ; runtime supplémentaire possible. Coût moyen à élevé pour une couverture commune. | Complément pour inspecter et agir, pas unique contrat d'exécution de wf. |
| WebDriver BiDi (spécification W3C) | Protocole bidirectionnel de contrôle des navigateurs décrit par le groupe W3C Browser Testing and Tools. | Niveau de support inégal selon CEF, WebKitGTK, WKWebView, WebView2 et navigateur système, à qualifier moteur par moteur ; processus ou passerelle supplémentaires. Coût moyen à élevé. | À qualifier par moteur ; pas de dépendance dans le premier incrément. |
| WebMCP (outils déclarés par la page) | La preview pourrait publier des outils adaptés à l'agent, sans sélecteurs fragiles. | Draft Community Group Report, hors Standards Track W3C ; les outils viennent de la page, donc d'une source non fiable dans le modèle de menace ci-dessous. Coût inconnu. | À surveiller ; ne confère aucun droit et n'est pas adopté. |
| Export du bridge ou terminal brut | Rapide à expérimenter, fonctions existantes immédiatement accessibles. | Autorité native trop large, types bas niveau, parsing de texte, appels arbitraires, aucune politique commune fiable. | Écarté comme interface publique par défaut. |
| Service distant ou multi-utilisateur | Machines provisionnées et automatisation centralisée. | Identité, TLS, isolation entre utilisateurs, secrets, facturation/quotas et exploitation ; change le modèle de confiance. Coût très élevé. | Hors premier incrément ; décision dédiée. |

MCP sur WebSocket propriétaire n'est pas adopté : le WebSocket du bridge n'est pas un transport
MCP. JSON et JSON Schema se terminent dans l'adaptateur ; aucun nouveau type d'objet n'est ajouté
au codec binaire. Les règles d'évolution de l'ADR-003 restent applicables.

## Décision

### Contrat v1 et moteur commun

`include/webfront/automation/Contract.hpp` décrit les opérations, entrées, résultats, erreurs et
permissions. CLI, catalogue MCP et validations natives sont projetés depuis cette source unique.
`Session.hpp` possède les runs, leurs identifiants, délais, états, résultats et l'audit ; la borne des résultats
est appliquée avant `complete` par le chemin `connectAutomation`, pas par `Session`.
Le terminal et l'agent réutilisent runNonInteractive et le même moteur de build/check/test ;
l'agent est soumis en plus aux grants natifs, sans exporter toute l'autorité du terminal humain.

Le premier contrat expose check, build, test, preview, inspect, click et fill. Il distingue les
diagnostics structurés, la sortie, le rapport de tests et les données d'inspection. Une opération
longue rend un runId consultable ; le chemin `connectAutomation` borne les sorties (queue de 16 000 unités) et signale
les coupures par `truncated`, alors que `runNonInteractive` conserve par défaut sa sortie complète. L'annulation est coopérative,
ne revient pas sur une publication commise et ne garantit pas d'arrêter le JavaScript déjà lancé.
Un résultat perdu après une mutation est signalé comme incertain, jamais rejoué automatiquement.
wf run/exec et callbacks C++ arbitraires ne sont pas des outils MCP. Amendement (#466) : `init`, `create`, `install`,
`restore` et `embed` sont des opérations du contrat, chacune sous sa propre permission explicite du propriétaire ;
`install`/`restore` n'utilisent que le registre déjà configuré (jamais choisi par l'agent) et les arguments restent
des noms simples ou des chemins relatifs au projet.

### CLI et MCP local stdio

`webfront-agent` est activé explicitement à la construction. La CLI fournit JSON, --schema et
--agent-help. L'adaptateur natif implémente un profil limité de MCP 2025-11-25, sans SDK de production
ni runtime hôte supplémentaire ; le profil et ses limites sont dans [AUTOMATION](../AUTOMATION.md).
Quatre outils : webfront.session, webfront.start, webfront.result, webfront.cancel. stdout est
exclusivement protocolaire en mode MCP ; stderr reçoit les diagnostics.

Une version de protocole demandée non supportée reçoit la version prise en charge, conformément
à la négociation MCP ; le client décide de poursuivre. Les requêtes start sont courtes et rendent
un runId : notifications/cancelled ne remplace pas webfront.cancel pour cette opération de fond.
L'implémentation maintenue est éprouvée par le client Python standard du dépôt et par le SDK Python
officiel 2.3.0 dans un environnement de test séparé. Ce SDK n'est pas installé par configure.

### Attachement à la même fenêtre et contrôle de la preview

Le propriétaire lance --serve --listen DIRECTORY avec projet et grants explicites. Les clients
CLI/MCP s'attachent au même contrôleur ; leur déconnexion ne ferme pas la fenêtre. Le canal natif
est un JSON-line TCP 127.0.0.1 à port OS, et non MCP HTTP. Une capacité OS de 256 bits, stockée dans
un répertoire privé hors projet, authentifie chaque requête. Le lecteur POSIX épingle un descripteur
de répertoire vérifié puis utilise openat ; Windows refuse les reparse points et dépend de l'ACL
du répertoire. Les sockets ne sont pas héritables. SIGINT/SIGTERM, arrêt normal et watchdog révoquent
le canal ; un arrêt non interceptable ne peut pas exécuter de nettoyage.

Le navigateur autorisé revendique un secret de lancement à usage unique dans le fragment d'URL,
retiré avant revendication. La capacité du canal natif n'est jamais transmise à la page. Les callbacks
page utilisent des preuves HMAC-SHA-256 avec anti-rejeu et réponses authentifiées. Cela ne transforme
pas le projet et sa frame de même origine en principaux de sécurité isolés : un projet hostile
peut modifier le runtime parent. Les refus sont décidés nativement avant validation des arguments.

Le terminal indique l'action de l'agent et offre l'arrêt. Un verrou commun refuse les mutations
concurrentes humaines/agentes. Une annulation ne libère pas ce verrou tant que le travail réel,
y compris imbriqué, continue. Un wf dev en attente de modifications ne conserve pas le verrou ;
compilation/publication le reprennent. L'audit conserve les décisions d'autorisation de start et
les runs conservent leurs résultats séparément, avec une rétention de 64 entrées chacun.

L'inspection livre un DOM réduit borné des éléments visibles à ID explicite, les erreurs observées
et des cibles sémantiques. Click/fill exigent la révision de page ; une navigation invalide les
anciennes cibles. Les mots de passe, fichiers, valeurs de formulaires et HTML intégral ne sont pas
exportés par inspect. Il n'y a ni eval, shell, export automatique des callbacks, capture d'écran
ni garantie d'arbre d'accessibilité complet.

### Modèles locaux et premier consommateur

Le contrôle d'accès ne dépend pas de la qualité du raisonnement. Le catalogue est petit, les
sorties bornées et les refus récupérables. Un hôte traduit les outils MCP pour son moteur d'inférence ;
la CLI reste une alternative. L'adaptateur de mesure LM Studio utilisé pour les cinq modèles
n'est pas une nouvelle interface maintenue : le banc fourni parle toujours Ollama.

La qualification [du 2026-10-29](../validation/automation-models-2026-10-29.md) porte sur deux tâches
synthétiques, trois répétitions et cinq modèles déjà installés. Les 30 sources finales passent les
vérifications indépendantes ; 29 conversations ont elles-mêmes observé check et test réussis.
L'essai Flint incomplet reste un échec de cette seconde métrique. Coût du catalogue selon le modèle :
553 à 837 tokens. Le rapport précise durées, configuration, données par essai et limites ; il ne
mesure pas le démarrage complet et ne qualifie pas la résistance à l'injection de prompt.

Pour #402, `webfront_add_jasmine_test` et `webfront::testing::JasmineTest` exécutent le Jasmine
verrouillé dans la vraie page applicative. Cible, frontend, specs précompilées, arguments et
watchdog extérieur au renderer sont explicites. Échec, zéro spec, crash, fermeture prématurée et
résultat manquant donnent un échec CTest. Voir [CONSUMER_TESTING](../CONSUMER_TESTING.md).

## Modèle de menace et risques résiduels

Les actifs sont le projet et ses artefacts, les fichiers du compte utilisateur, secrets de registre
et de CI, autorité des callbacks C++, sessions et éventuels périphériques. Les entrées adverses
incluent projet/paquet compromis, texte de page ou logs hostiles, site web tiers, client local non
autorisé et processus local compromis. Les frontières sont client/contrôleur, contrôleur/renderer,
projet/disque, application/callbacks et réseau/registres. Un processus hostile sous le même compte
ou un OS compromis ne peut être déclaré isolé par les seules permissions MCP.

Les niveaux ci-dessous décrivent l'impact potentiel, sans estimation quantitative de probabilité.
Les exigences sont rapprochées des protections livrées dans le bilan ci-dessous ; elles ne valent
pas certification contre un code de projet hostile.

| Menace et gravité | Mesures exigées | Risque résiduel |
| --- | --- | --- |
| Injection de prompt dans sources, page, diagnostics, descriptions d'outils ou outils déclarés par la page — élevée | Traiter les contenus comme données ; outils/schémas de confiance ; autorisations hors du projet, vérifiées côté contrôleur à chaque appel. Une annotation MCP n'accorde aucun droit. Les outils déclarés par une page ne sont jamais ajoutés au catalogue de confiance. | L'agent peut encore choisir une action autorisée nuisible ; séparer l'autorité de la qualité du raisonnement. Les tâches synthétiques mesurées ne qualifient pas la résistance aux injections de prompt. |
| Exécution arbitraire par script, test, paquet ou callback — critique | Aucune exportation automatique du bridge ; capacités séparées pour exécution, écriture et réseau ; politique plafonnée par le lanceur, non extensible par le manifeste. | Le code exécuté garde l'autorité accessible au processus. Projet hostile : isolation OS/CI dédiée et secrets retirés ; aucun sandbox WebFront annoncé. |
| Accès non autorisé ou détournement de session — critique | Stdio ou IPC protégé ; attachement explicite et révocable ; autorisation liée à projet/session ; identifiants non assimilés à une identité. | Un processus du même utilisateur peut dépasser les garanties du canal ; documenter cette limite. |
| DNS rebinding, requêtes web et exposition réseau — sans objet tant que HTTP n'est pas retenu, critique s'il l'est | Loopback uniquement, validation Host et Origin, refus d'Origin invalide, authentification indépendante même sans Origin ; pas de CORS permissif. Aucun accès distant implicite. | Origin n'authentifie pas un client natif ; la politique du bridge ADR-008 ne sécurise pas à elle seule MCP. |
| Chemins hostiles, liens symboliques et courses — élevée | Projet explicitement lié, résolution et contrôle au moment de l'accès, traitement des liens et opérations atomiques quand nécessaires ; cas de course testés. | NativeDebugFS et permissions de manifeste n'isolent pas du code exécuté avec les droits OS du processus. |
| Exfiltration de secrets et SSRF — critique | Lectures/sorties bornées et aucun passage automatique de tokens ; le lanceur externe doit retirer les secrets de l'environnement et imposer la politique réseau. L'environnement hérité n'est pas automatiquement expurgé. | Logs, DOM et captures peuvent contenir des secrets. L'expurgation seule ne protège pas d'un code hostile ; isolation et contrôle réseau externes nécessaires. |
| Renderer bloqué, sorties illimitées, concurrence — élevée | Watchdog natif, quotas et contrôle de flux, sérialisation des mutations, fermeture et drainage déterministes ; arrêt forcé par superviseur si le processus ne se termine pas. | Une interruption peut laisser un effet déjà commis ; distinguer arrêté, terminé et résultat inconnu. |
| Compromission du bootstrap ou activation en production — critique | Versions/intégrités épinglées, dépendances MCP revues, activation explicite ; l'application embarquée ordinaire ne sert pas de callbacks ni de canal d'automatisation. Le module automation de l'environnement développeur reste inactif sans le lanceur optionnel. | Un artefact valide peut être malveillant ; l'intégrité n'est pas une revue de comportement. |

Une session de confiance accordée au lancement peut éviter des confirmations répétitives. Les actions
dépassant ses capacités sont refusées ou requièrent une extension explicite depuis un canal humain
de confiance ; un contenu de page ne peut pas accorder cette extension. Les scripts applicatifs
pilotant du matériel réel demandent une analyse propre aux effets physiques ; seul un firmware
émulé entre dans le cas initial #402.

## Bilan d'acceptation et conséquences

Le contrat, l'attachement et la qualification demandés sont livrés. Le coût accepté est un
adaptateur de protocole limité à maintenir, un contrôleur natif supplémentaire et une politique
explicite de permissions. L'acceptation n'exige pas une réussite parfaite de tous les modèles :
les échecs de raisonnement restent visibles et ne doivent pas augmenter leur autorité.

| Critère du premier incrément | Preuves et réserves |
| --- | --- |
| Comparaison, contrat, décisions et menace | #405, #407 ; Contract.hpp, Session.hpp, AUTOMATION.md et ce bilan. |
| Vraie page Jasmine/CTest indépendante de MCP | #406/#402 ; tests consommateurs pass/fail/empty/pending/crash/close/hang, Linux/Xvfb et moteurs natifs. |
| CLI, fenêtre et clients MCP | AutomationMcpWindow, AutomationSdkWindow (SDK officiel 2.3.0 déjà installé), CLI et consignes ; catalogue limité, SDK de production non requis. |
| Modèle local, tokens et tâches de correction | Rapport et JSON versionnés : 30/30 sources finales, 29/30 conversations complètes, cas incomplet explicite ; aucune garantie générale. |
| Attachement, refus, arbitrage et preview | AutomationTests.cpp, AutomationChannelTests.cpp, automation-specs.mjs et scénario réel à deux clients. |
| Arrêt, ressources, renderer bloqué et sorties | AutomationMcpWatchdog et AutomationCliWatchdog : deadlines natives, EOF, nettoyage/réutilisation d'endpoint et échec Unicode ; annulation sans libération anticipée du verrou. |
| Isolation réellement promise | Grants natifs et capacité privée ; aucune sandbox de projet ni isolation du même utilisateur. Profils temporaires CEF Linux/WebView2, magasins partagés WKWebView/WebKitGTK. |
| Validation de la tête fusionnée | [Desktop](https://github.com/ambroise-leclerc/WebFront/actions/runs/40925937329), [CEF GCC/Clang et sanitizers](https://github.com/ambroise-leclerc/WebFront/actions/runs/40925952697), [paquets](https://github.com/ambroise-leclerc/WebFront/actions/runs/40925937495), tous sur 4a1c92b. |
| Démarrage, exécution et arrêt | [Mesures locales de réception](../validation/automation-acceptance-2026-10-29.md) : trois processus WKWebView distincts, préparation exclue, prêt et fermeture réellement observés. |
| Distribution | Sources 0.6.3 ; reconstruction répétée et pin préparés séparément. Déploiement puis vérification avant bascule du pin, sans réécrire une version publiée. |

Les mesures de démarrage/arrêt de réception sont distinctes des latences de tâches des modèles.
La disponibilité et l'arrêt sont aussi vérifiés par les scénarios de processus et leurs échéances. Une mesure de capacité
ou de performance ne constitue pas une qualification universelle de sécurité. Les durées de CI ne sont
pas utilisées ici comme mesure de performance.

MCP HTTP, accès distant/multi-utilisateur, WebDriver BiDi, WebMCP, captures et installation de modèles
restent différés ; les installer ou les exposer demande une décision propre. L'autorité héritée,
les secrets de l'environnement et le contrôle réseau restent à la charge du lanceur externe.
La fermeture de #403/#404 constate ce premier incrément accepté, pas la livraison de ces extensions.

## Références et portée des sources

- [Automatisation](../AUTOMATION.md), [Jasmine consommateur](../CONSUMER_TESTING.md),
  [qualification et données](../validation/automation-models-2026-10-29.md),
  [publication](../../js/developer/tools/PUBLISHING.md).
- ADR-003/004/006/008 et ADR-012 à 016 conservent leurs contraintes de protocole, d'arrêt,
  de confiance et de distribution. Cette décision ne modifie pas le codec binaire.
- [MCP 2025-11-25, transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
  et [négociation](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle#version-negotiation).
- [MCP, bonnes pratiques de sécurité](https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices).
- [WebDriver BiDi, source du projet W3C](https://github.com/w3c/webdriver-bidi/blob/master/index.bs) :
  protocole de contrôle bidirectionnel ; sa présence ne prouve pas sa disponibilité dans tous nos moteurs.
- [WebMCP, Draft Community Group Report](https://webmachinelearning.github.io/webmcp/) :
  outils JavaScript de page ; hors W3C Standards Track, non adopté ici.

Les références MCP, BiDi et WebMCP ont été revérifiées le 2026-10-29. Le choix WebFront, ses
frontières et ses limites sont cette décision, pas une certification de conformité par ces sources.
