# ADR-019 : Rendre l'environnement développeur automatisable par des agents

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-28 | Proposée | Ambroise Leclerc | Comparaison des voies d'automatisation, orientation MCP locale et modèle de menace de l'épique #403 ; #402 comme premier cas consommateur. | À renseigner après intégration. |

## État et portée

**Proposée le 2026-10-28**, dans l'[épique #403](https://github.com/ambroise-leclerc/WebFront/issues/403).
Ce document propose une architecture et des critères d'acceptation ; il ne livre ni serveur MCP,
ni pilote public, ni isolation de code hostile. Les noms d'opérations ci-dessous sont des intentions,
pas de nouvelles API disponibles. L'acceptation exige prototypes, revue de sécurité et preuves.

## Contexte vérifié

L'[issue #402](https://github.com/ambroise-leclerc/WebFront/issues/402) demande de lancer les specs
Jasmine de SiliconForge dans sa vraie page applicative, avec son propre `documentRoot`, depuis CTest.
Elle fournit un premier cas d'automatisation concret : le résultat doit être exploitable sans
intervention humaine, sans runner qui imite Jasmine et sans dépendance à une session préexistante.

L'environnement développeur possède déjà `runNonInteractive(argv, ...)`. Le pilote interne
`webfront-wf`, sous `test/driver/`, lui transmet des commandes et récupère statut, sortie et rapport
JSON. Ses watchdogs natifs bornent l'exécution même si la page cesse de répondre. Le rapport détaillé
concerne actuellement `wf test` ; les autres commandes n'offrent pas toutes des résultats sémantiques
équivalents. Cette base n'est pas un contrat public stabilisé pour un consommateur.

Le terminal, la compilation et les tests utilisent l'environnement téléchargé, épinglé et vérifié.
`wf test` exécute les specs dans une frame de l'environnement ; il ne faut pas confondre cette voie
avec l'instrumentation de la page complète d'une application consommatrice. Une boucle synchrone
dans une frame de même origine peut bloquer le renderer et ses délais JavaScript.

L'objectif dépasse le test : un agent doit découvrir une session, utiliser la fenêtre de
développement, lancer check/build/test, lire des diagnostics, inspecter une preview, puis vérifier
une correction. Le développeur conserve la visibilité et le contrôle de cette même session.

## Contraintes conservées

- Cœur C++23 header-only ; outils de développement optionnels, hors des assets minimaux fermés
  de l'ADR-016. Aucun serveur MCP actif dans une distribution ordinaire.
- Aucun runtime JavaScript ni gestionnaire de paquets requis sur la machine hôte. Un SDK MCP
  exigeant Node ou Python ne devient pas implicitement une dépendance du parcours utilisateur.
- Origines restreintes, contenu de confiance et absence de sandbox (ADR-008). Les droits du
  manifeste ne constituent pas une frontière d'isolation des scripts ni des callbacks natifs.
- Bootstrap et paquets verrouillés par intégrité ; tests reproductibles sans réseau public après
  provisioning. L'intégrité des octets ne prouve pas leur innocuité.
- Bridge asynchrone et borné (ADR-003/004), tâches possédées et drainées (ADR-006). Une annulation
  locale d'appel ne stoppe pas le JavaScript déjà lancé ; ne pas promettre de rollback général.
- Le contrôle passe par des adaptateurs de frontend ; CDP n'est pas une hypothèse commune aux
  moteurs CEF, WebKitGTK, WKWebView, WebView2 et au navigateur système.

## Possibilités comparées

Les coûts sont des estimations qualitatives d'architecture, à confirmer par prototype ; aucune
mesure de performance ou de compatibilité MCP n'est revendiquée ici.

| Voie | Intérêt | Limites et coût | Position proposée |
| --- | --- | --- | --- |
| CLI structurée issue du pilote interne | Simple pour CI, CTest et scripts ; réutilise le provisioning et les watchdogs. | Publiciser packaging, schémas et cycle de vie ; la CLI seule décrit mal une fenêtre partagée et ses événements. Coût moyen. | Socle à stabiliser, utilisable sans agent ni MCP. |
| MCP local sur stdio | Découverte des outils et schémas par les clients compatibles ; processus lancé par le client, sans endpoint MCP réseau. | Adaptation du protocole, packaging, stdout exclusivement protocolaire, compatibilité clients ; accès au processus toujours sensible. Coût moyen au-dessus du socle. | Première voie MCP à prototyper. |
| MCP Streamable HTTP local | Service persistant, plusieurs clients, attachement naturel à une fenêtre existante. | Authentification, Host/Origin, révocation, connexions et sessions ; nouveau service réseau privilégié. Coût élevé. | Différé jusqu'à preuve d'un besoin non couvert par stdio et IPC natif. |
| Pilotage navigateur : accessibilité/DOM, captures, Playwright ou CDP | Vérifie l'IHM visible et les interactions ; outils génériques utiles aux agents. | Sélecteurs fragiles, captures coûteuses et sensibles, fonctions différentes selon moteur ; runtime supplémentaire possible. Coût moyen à élevé pour une couverture commune. | Complément pour inspecter et agir, pas unique contrat d'exécution de wf. |
| Export du bridge ou terminal brut | Rapide à expérimenter, fonctions existantes immédiatement accessibles. | Autorité native trop large, types bas niveau, parsing de texte, appels arbitraires, aucune politique commune fiable. | Écarté comme interface publique par défaut. |
| Service distant ou multi-utilisateur | Machines provisionnées et automatisation centralisée. | Identité, TLS, isolation entre utilisateurs, secrets, facturation/quotas et exploitation ; change le modèle de confiance. Coût très élevé. | Hors premier incrément ; décision dédiée. |

MCP sur WebSocket propriétaire n'est pas retenu : le bridge WebFront n'est pas un transport MCP
standard. JSON et schémas MCP se terminent dans l'adaptateur ; ils ne deviennent pas un nouveau
type générique d'objet traversant le protocole binaire existant. Tout changement de ce protocole
exigerait l'évolution et les tests conjoints prescrits par ADR-003.

## Décision proposée

### Un contrat d'opérations partagé, puis des adaptateurs

Séparer les opérations développeur du rendu du terminal. La fenêtre, la CLI publique future,
CTest et MCP appellent le même dispatch, les mêmes validations et les mêmes permissions. Réutiliser
`runNonInteractive` et le pilote lorsque leur contrat convient ; éviter une seconde implémentation
de build/test dans le serveur MCP et le parsing de sorties destinées à l'humain.

Un contrôleur natif possède les sessions, exécutions et watchdogs, sans déplacer nécessairement
la compilation hors du navigateur. Le lancement explicite fixe le projet et les capacités autorisées.
Un identifiant de session n'est pas un secret et ne confère aucun droit. Les opérations ciblent une
session et une page explicites ; aucune sélection implicite de la dernière fenêtre ouverte.

Pour chaque opération, définir schéma/version, capacités requises, entrées validées, effet,
événements de progression, résultat final, erreurs et comportement à l'arrêt. Les intentions
initiales sont : découverte du projet et des capacités ; check/build/test ; lancement et observation
de preview ; lecture bornée des diagnostics ; état d'une exécution et demande d'arrêt.
Installation/restauration réseau, scripts `wf run`/`exec`, commandes C++ applicatives et écritures
reçoivent des capacités séparées. Un lancement de tests est une exécution de code de projet,
pas une permission de lecture seule.

Les exécutions longues retournent un identifiant et un état consultable, puis un résultat corrélé.
Définir l'adaptation à la progression et à l'annulation MCP dans la révision négociée, sans imposer
une extension expérimentale pour terminer un build. Ne pas relancer automatiquement une opération
à effets après perte de connexion ; retrouver son état ou signaler l'incertitude. Borner files,
durées, concurrence et sorties, indiquer la troncature et séparer diagnostics structurés et logs.

### MCP optionnel local, fenêtre contrôlable

Prototyper un adaptateur stdio natif et optionnel. Le client lance le processus ; stdout ne contient
que les messages MCP, stderr les diagnostics. Comparer une implémentation limitée du protocole
avec un SDK C++ compatible, sa licence, sa maintenance et ses dépendances avant de choisir. Négocier
une révision précise et publier la matrice des clients réellement vérifiés.

Le premier mode démarre sa propre session autorisée. L'attachement à une fenêtre déjà ouverte
nécessite un canal IPC natif authentifié, aux permissions OS restrictives, et une autorisation
révocable limitée au projet/session. Choisir et tester le mécanisme par plateforme avant livraison ;
ne pas mettre de secret dans le DOM, une URL ou les journaux. Si l'IPC ne convient pas, rouvrir
explicitement l'option Streamable HTTP avec ses contrôles, sans exposer le bridge comme substitut.

La fenêtre affiche les opérations et leur état et permet l'arrêt. Arbitrer les mutations humaines
et agentes (sérialisation ou rejet explicite), avec révision du projet pour éviter les écritures
sur un état périmé. Un journal borné conserve identité de session, opération, décision d'autorisation
et résultat, avec politique d'expurgation et de rétention.

Pour la preview, explorer état prêt, erreurs console, arbre accessible ou DOM réduit et actions
sémantiques validées. Identifier page, version et cible pour éviter une action après navigation.
Les captures sont explicites et bornées. Ni `eval`, ni shell généraliste, ni export automatique des
callbacks ne sont nécessaires au premier contrat. Une API métier d'automatisation telle que celle
de SiliconForge reste un choix explicite du consommateur et doit recevoir sa propre politique.

### #402 livre une première capacité indépendante

Stabiliser une intégration publique Jasmine/CTest pour un consommateur qui sert sa propre page.
La fonction CMake proposée dans #402 reste à concevoir : cible, arguments, specs, frontend, page
et préparation de Jasmine doivent être explicites. Conserver Jasmine verrouillé et provisionné
comme outil optionnel, sans l'ajouter aux ressources du cœur.

Le runner démarre l'application, attend son état prêt, exécute les specs dans le contexte convenu,
collecte le résultat et ferme ses ressources. Des ports/profils distincts isolent les tests parallèles.
Échec, absence de specs exécutées, crash, résultat manquant et timeout échouent dans CTest. Un
watchdog extérieur au renderer borne les blocages synchrones. #402 peut être livré avant MCP.

## Modèle de menace et risques résiduels

Les actifs sont le projet et ses artefacts, les fichiers du compte utilisateur, secrets de registre
et de CI, autorité des callbacks C++, sessions et éventuels périphériques. Les entrées adverses
incluent projet/paquet compromis, texte de page ou logs hostiles, site web tiers, client local non
autorisé et processus local compromis. Les frontières sont client/contrôleur, contrôleur/renderer,
projet/disque, application/callbacks et réseau/registres. Un processus hostile sous le même compte
ou un OS compromis ne peut être déclaré isolé par les seules permissions MCP.

Les niveaux ci-dessous décrivent l'impact potentiel, sans estimation quantitative de probabilité.
Les mesures sont des exigences à implémenter, pas des protections aujourd'hui livrées.

| Menace et gravité | Mesures exigées | Risque résiduel |
| --- | --- | --- |
| Injection de prompt dans sources, page, diagnostics ou descriptions d'outils — élevée | Traiter les contenus comme données ; outils/schémas de confiance ; autorisations hors du projet, vérifiées côté contrôleur à chaque appel. Une annotation MCP n'accorde aucun droit. | L'agent peut encore choisir une action autorisée nuisible ; séparer l'autorité de la qualité du raisonnement. |
| Exécution arbitraire par script, test, paquet ou callback — critique | Aucune exportation automatique du bridge ; capacités séparées pour exécution, écriture et réseau ; politique plafonnée par le lanceur, non extensible par le manifeste. | Le code exécuté garde l'autorité accessible au processus. Projet hostile : isolation OS/CI dédiée et secrets retirés ; aucun sandbox WebFront annoncé. |
| Accès non autorisé ou détournement de session — critique | Stdio ou IPC protégé ; attachement explicite et révocable ; autorisation liée à projet/session ; identifiants non assimilés à une identité. | Un processus du même utilisateur peut dépasser les garanties du canal ; documenter cette limite. |
| DNS rebinding, requêtes web et exposition réseau — critique si HTTP ajouté | Loopback uniquement, validation Host et Origin, refus d'Origin invalide, authentification indépendante même sans Origin ; pas de CORS permissif. Aucun accès distant implicite. | Origin n'authentifie pas un client natif ; la politique du bridge ADR-008 ne sécurise pas à elle seule MCP. |
| Chemins hostiles, liens symboliques et courses — élevée | Projet explicitement lié, résolution et contrôle au moment de l'accès, traitement des liens et opérations atomiques quand nécessaires ; cas de course testés. | NativeDebugFS et permissions de manifeste n'isolent pas du code exécuté avec les droits OS du processus. |
| Exfiltration de secrets et SSRF — critique | Environnement hérité minimisé, lectures/sorties bornées, réseau et endpoints explicitement autorisés, contrôle des redirections ; aucun passage automatique de tokens. | Logs, DOM et captures peuvent contenir des secrets. L'expurgation seule ne protège pas d'un code hostile ; isolation et contrôle réseau externes nécessaires. |
| Renderer bloqué, sorties illimitées, concurrence — élevée | Watchdog natif, quotas et contrôle de flux, sérialisation des mutations, fermeture et drainage déterministes ; arrêt forcé par superviseur si le processus ne se termine pas. | Une interruption peut laisser un effet déjà commis ; distinguer arrêté, terminé et résultat inconnu. |
| Compromission du bootstrap ou activation en production — critique | Versions/intégrités épinglées, dépendances MCP revues, activation explicite ; test de distribution sans callbacks, canal ni outils d'automatisation. | Un artefact valide peut être malveillant ; l'intégrité n'est pas une revue de comportement. |

Une session de confiance accordée au lancement peut éviter des confirmations répétitives. Les actions
dépassant ses capacités sont refusées ou requièrent une extension explicite depuis un canal humain
de confiance ; un contenu de page ne peut pas accorder cette extension. Les scripts applicatifs
pilotant du matériel réel demandent une analyse propre aux effets physiques ; seul un firmware
émulé entre dans le cas initial #402.

## Conséquences et conditions d'acceptation

Le gain attendu est un seul parcours vérifiable pour humain, CI et agent, avec diagnostics exploitables
et automatisation de la vraie fenêtre. Les coûts acceptés seraient un contrat à versionner, un
packaging optionnel, des adaptateurs moteurs et clients à maintenir, une politique d'autorisation
et un superviseur natif. MCP ne remplace ni les tests ni la sécurité du système hôte.

Avant acceptation :

- Livrer #402 et un consommateur externe de régression, sans runner Jasmine de substitution ;
  tester réussite, échec, zéro spec, crash, timeout, exécutions parallèles et nettoyage sous Linux sans affichage.
- Démontrer par prototype le même check/build/test dans la fenêtre, la CLI et deux clients MCP
  identifiés ; mesurer démarrage, coût des sorties et arrêt, et consigner dépendances et versions.
- Démontrer démarrage autonome, choix de session et attachement refusé sans autorisation ;
  révocation, fermeture, déconnexion, renderer bloqué et arrêt pendant publication.
- Éprouver chemins hostiles, limites, injection de contenu, refus de capacités, absence de secrets
  dans logs/URLs et impossibilité d'étendre ses droits en modifiant le projet. Si HTTP est retenu,
  ajouter authentification, Host/Origin, rejeu et séparation des sessions aux tests négatifs.
- Vérifier séparation des projets et arbitrage humain/agent, résultats tronqués explicites et
  absence des capacités en distribution. Documenter les risques résiduels sans promesse d'isolation.
- Exécuter build/CTest CEF-off complet, CEF/Xvfb pour les changements navigateur/bridge, et les
  moteurs natifs concernés. Tests sans réseau public après provisioning ; dispatch CI requis
  selon AGENTS.md avant fusion des changements concernés vers develop.

Restent à décider par prototype : SDK ou implémentation limitée, version MCP, schémas publics,
packaging du pilote, mécanisme IPC par OS, granularité des permissions, capacités d'inspection par
moteur et besoin réel d'HTTP. L'accès distant/multi-utilisateur et le shell généraliste sont hors
du premier incrément. L'ADR reste proposée tant que ces choix et preuves ne sont pas consignés.

## Références et portée des sources

Constats locaux : [environnement développeur](../DEVELOPER_ENVIRONMENT.md),
[pilote](../../test/driver/WebFrontWf.cpp), [ADR-008](ADR-008-contenu-de-confiance-et-origines.md),
[ADR-012](ADR-012-preuves-et-tests.md), [ADR-013](ADR-013-bootstrap-developpeur-a-la-demande.md),
[ADR-014](ADR-014-construction-javascript-dans-le-browser.md) et
[ADR-016](ADR-016-bootstrap-comme-socle.md).

Sources externes consultées le **2026-10-28** :

- [MCP, transports, révision 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports) :
  stdio et Streamable HTTP, séparation des logs et du protocole, contrôles d'origine et recommandations d'authentification.
- [MCP, bonnes pratiques de sécurité](https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices) :
  compromission des serveurs locaux, sessions, SSRF, délégation d'autorité et minimisation des droits.

Ces références décrivent MCP ; le découpage WebFront, la préférence pour stdio, les contrôles IPC
et les niveaux de risque sont les propositions de cette ADR, et non une certification MCP.
