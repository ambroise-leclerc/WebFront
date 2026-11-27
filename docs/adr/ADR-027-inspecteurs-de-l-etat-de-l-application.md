# ADR-027 : Inspecteurs de l'état de l'application

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.3 | 2026-11-27 | Proposée | Ambroise Leclerc | État du pont dans la page (#684, section 8) : sonde `probe` de `WebFront.js`, posée seulement pendant qu'un inspecteur lit ; moniteur de la page hôte, qui suit le pont de l'interface ; relais `webfront.inspector.bridgeSource`, `publishBridge` et `bridge`, sous la règle d'accès de la section 2, où la page ne publie que pendant qu'un panneau détaché lit (bail de 15 s) ; état borné à 256 lignes de textes de 256 octets, pour 16 pages au plus. | À renseigner après intégration. |
| 1.2 | 2026-11-26 | Proposée | Ambroise Leclerc | Registre introspectable (#681) : pages `webfront.inspector.functions`, `jsFunctions` et `browsers` (section 7) ; enregistrements tenus par les gestionnaires, donc retirés avec une fonction d'espace ; fonctions `webfront.inspector.*` non listées ; durée mesurée pendant une fenêtre de 5 s après chaque lecture des fonctions ; appels vers JavaScript comptés par nom, 1 024 noms au plus, seulement une fois l'environnement monté ; `BasicWF::registeredFunctions()` pour l'application. | À renseigner après intégration. |
| 1.1 | 2026-11-25 | Proposée | Ambroise Leclerc | Hôte détachable (#680) : `webfront.inspector.embedded()` dit à la page hôte si elle peut tourner dans un frontend intégré (vue prouvée, ou frontend actif qui transmet la preuve), ce qui décide de l'onglet du navigateur système ; `webfront.inspector.closeWindow(panneau, hôte)` ferme la fenêtre d'un panneau rattaché ; une fenêtre par panneau et par page hôte, qui seule le rattache ; choix et place conservés dans `inspector-window.json`, lus avec le jeton de leurs octets par `webfront.inspector.readPreferences()`. | À renseigner après intégration. |
| 1.0 | 2026-11-25 | Proposée | Ambroise Leclerc | Décision initiale (#678, épique #677) : inspecteurs réservés à l'environnement développeur, accès réservé aux vues prouvées sauf autorisation explicite, nœuds d'arbre sur les types existants du pont, lecture et différences paginées, opérations entières sous le verrou ou sur l'exécuteur de l'application, modification à la demande de l'application, page unique `inspector.html` dans un conteneur choisi sur l'accès effectif. | À renseigner après intégration. |

## État et portée

**Proposée le 2026-11-25.** Elle précède tout code de l'épique #677 (#679 à #686), dont elle fixe les contrats. Elle
ne modifie pas le format binaire du pont ([ADR-003](ADR-003-protocole-binaire-et-bornes.md)), ni la liste des
ressources embarquées du cœur ([ADR-016](ADR-016-bootstrap-comme-socle.md)), dont elle amende seulement la portée des
permissions (version 1.7 : écritures de l'environnement gardées quand une vue est prouvée, section 2), ni le
contrôle d'origine du pont ([ADR-008](ADR-008-contenu-de-confiance-et-origines.md)). Elle reprend tel quel le prédicat
d'association des vues de l'[ADR-020](ADR-020-services-systeme.md) (règle 3), et l'applique aux inspecteurs et aux
écritures de l'environnement.

Ce n'est pas un bac à sable : un inspecteur montre et, si l'application le permet, modifie l'état d'un processus de
développement. Il ne protège pas cet état contre un script admis dans la page (ADR-008).

## Contexte

L'onglet WebFront de l'environnement développeur (`bootstrap("developer")`) ouvre aujourd'hui le terminal flottant.
L'épique #677 lui ajoute un menu contextuel qui ouvre deux inspecteurs :

- **État C++** : fonctions enregistrées par `cppFunction` (signature, appels, échecs), fonctions JavaScript appelées
  par C++, navigateurs connectés, tâches en cours, commandes de l'application, et surtout des **observateurs** : le
  contenu d'un objet ou d'un conteneur C++ désigné par l'application, explorable en arbre et rafraîchi pendant qu'on
  manipule l'interface ;
- **État TypeScript** : l'arbre des paquets verrouillés (ce que disent `wf list --tree`, `wf why` et `wf info`,
  [ADR-023](ADR-023-espace-de-noms-des-commandes-du-terminal.md)) et l'état du pont dans la page.

Plusieurs faits du code contraignent la conception :

- `BasicWF` garde ses callbacks dans une table à types effacés : seul le nom est connu (`hasCppFunction`).
- Le pont transporte des scalaires, des chaînes, des tuples, des tableaux typés, des `std::bitset<N>` et des listes
  homogènes non imbriquées (ADR-003) ; un objet C++ arbitraire ne le traverse pas.
- L'encodeur C++ place une liste dans le tampon de la politique (`functionCallBufferSize`, 32 Kio par défaut) : une
  liste trop longue lève `http::BufferOverflowException`.
- Une connexion est **associée à une vue** quand elle a présenté, à sa poignée de main, le secret du lancement
  (`ViewLaunch`). Seuls les frontends intégrés (WebKitGTK, WKWebView, WebView2, CEF) le transmettent, et seulement au
  cadre principal de leur vue. Le navigateur système ne le reçoit jamais : passé sur la ligne de commande ou dans l'URL,
  il serait divulgué par l'historique ou la liste des processus. `BasicWF::isLocalClient` vaut vrai pour une vue
  prouvée, ou pour un pair de boucle locale si l'application a choisi `WebFrontConfig::trustLoopbackClients`.
- `openSecondaryWindow` (#616) ouvre une deuxième fenêtre native servie par le même serveur, avec la preuve de vue,
  sous WebKitGTK, WKWebView et WebView2 ; le navigateur système et CEF la refusent (`canShowSecondaryWindow()`).
- `forms::inspector` sait déjà afficher les champs d'une `forms::Description` et ne renvoyer que ceux qui ont changé.
- Les ressources embarquées du cœur sont une liste fermée sous budget (ADR-016, `MinimalCoreAssets`).

## Décision

### 1. Périmètre : l'environnement développeur seulement

Les fonctions réservées `webfront.inspector.*` ne sont enregistrées que par `bootstrap("developer")`, avec les
callbacks `webfront.developer.*`. Une application qui ne monte pas l'environnement n'en a aucune, et une construction
de distribution (`WEBFRONT_DISTRIBUTION=1`) ne les compile pas.

L'API que l'application utilise pour désigner ce qu'elle montre (`app.watch`, section 5) reste disponible dans toutes
les constructions, pour qu'une application n'ait pas à entourer ses observateurs de `#if`. Hors environnement
développeur, et en distribution, `watch` rend une poignée vide : rien n'est enregistré ni lu. Dans l'environnement, un
observateur coûte une entrée dans une table tant qu'aucun inspecteur ne le lit.

Le préfixe `webfront.` ne réserve aujourd'hui que les codes d'erreur (`BridgeError::reservedPrefix`), pas les noms de
callbacks. Cette décision réserve les noms en `webfront.inspector.` : `cppFunction` les refuse à l'application
(`std::invalid_argument`), pour qu'une application ne puisse ni masquer une fonction de l'inspecteur ni s'en faire passer
pour une.

### 2. Accès : vues prouvées, autorisation explicite pour le reste

Chaque fonction `webfront.inspector.*`, sauf `containers` (section 8), vérifie la connexion appelante avec le
prédicat de l'ADR-020, règle 3 (`isLocalClient`) : la connexion est associée à une vue de l'application, ou l'application a explicitement choisi
`trustLoopbackClients` et le pair est sur la boucle locale. Sinon l'appel est refusé avec le code d'erreur
`webfront.inspector.denied` (codes d'erreur typés de l'ADR-003, version 1.18), avant toute lecture.

Il n'y a pas d'option propre aux inspecteurs. `trustLoopbackClients` exprime déjà la seule confiance en jeu : « un pair
de la boucle locale est à moi ». Elle n'est sûre que sur une machine à un seul utilisateur, sans tunnel ni proxy local.
Une seconde option qui dirait la même chose sur un autre périmètre ne ferait qu'ajouter un cas à vérifier.

**Avec le navigateur système, l'inspecteur est refusé par défaut.** Le frontend par défaut de `webfront::WebFront` ne
reçoit pas de preuve de vue (voir « Contexte »), et cette décision n'en invente pas. Le menu de l'onglet garde ses
entrées, mais elles disent pourquoi l'inspecteur est indisponible et comment l'activer : choisir un frontend intégré, ou
`trustLoopbackClients` en connaissance de cause. Un mécanisme qui donnerait une preuve au navigateur système, par
exemple un code à usage unique dans l'URL, demanderait une décision de sécurité distincte (course avec un autre
processus local qui lit la ligne de commande, historique, extensions).

**Frontière cohérente avec l'environnement développeur.** Exiger la preuve pour lire l'état ne protège rien si un
client non prouvé peut changer ce que la vue prouvée exécute. Or les callbacks `webfront.developer.*` qui écrivent
dans le projet ne demandaient que le contrôle d'origine, qu'un processus local falsifie (ADR-008) : un autre
utilisateur de la machine pouvait réécrire un script du projet, attendre que la vue le charge, puis lire les
inspecteurs et utiliser les services natifs de l'ADR-020 par la connexion de cette vue.

Cette décision ferme cette voie pour **tous les points d'entrée que WebFront fournit**, et fixe le contrat de ceux
que l'application fournit.

**Points d'entrée de WebFront.** Quand le frontend actif transmet la preuve de vue (WebKitGTK, WKWebView, WebView2,
CEF, et `NativeOrBrowserFrontend` tant qu'il n'est pas replié sur le navigateur système), les callbacks réservés qui
modifient ce que la vue exécute ou ce que le serveur sert, **ou qui exécutent du code de l'application capable de le
faire**, exigent le même prédicat que les inspecteurs (`isLocalClient`), sinon `webfront.inspector.denied` :

- `webfront.developer.begin`, `beginNew`, `write`, `commit`, `commitGroup` et `abort`, c'est-à-dire toute écriture
  dans le projet, dont les sources, le manifeste, le lock, les paquets installés et les publications de
  `.webfront/developer/` (environnement, aperçu, tests) ;
- `webfront.commands.start` et `webfront.commands.cancel` (`Commands.hpp`, ADR-023) : une commande C++ de
  l'application peut écrire dans le projet ou changer ce que le serveur sert, et WebFront ne sait pas ce qu'elle fait ;
  la lancer est donc traité comme une écriture. `webfront.commands.list`, qui ne rend que des noms et des aides, garde
  le contrôle d'origine. Les sorties d'une commande (`webfront.commands.output`, `exit`) vont à la connexion qui l'a
  lancée, déjà admise ;
- tout callback réservé ajouté plus tard qui écrit dans le projet, change ce que le serveur sert, ou exécute du code
  de l'application.

**Callbacks de l'application.** Un `cppFunction` que l'application enregistre sous son propre nom est hors de portée de
cette garde : WebFront ne sait pas ce qu'il fait, et le garder par défaut casserait toutes les applications servies à
un navigateur. Son contrat de sécurité est donc celui de l'application, et cette décision l'écrit : **un callback de
l'application qui écrit dans le projet ou dans ce que le serveur sert, qui lance un processus, ou qui expose un état
qu'elle ne montrerait pas à un client non prouvé, prend l'appelant en premier paramètre (`UI`) et vérifie
`ui.isLocalClient()` avant d'agir**. Sans cette vérification, il contourne la frontière ci-dessus, et c'est un défaut
de l'application, pas de WebFront. La documentation des inspecteurs, `docs/DEVELOPING_WITH_AGENTS.md` et le guide des
starters le disent ; le registre (#681) affiche, pour chaque `cppFunction`, s'il reçoit l'appelant, ce qui aide à
repérer les callbacks qui ne peuvent pas faire cette vérification.

Les lectures du projet (`webfront.developer.read`, `list`, `entries`, `digests`, `state`, `config`) gardent le contrôle
d'origine : elles ne changent rien à ce que la vue exécute. Le terminal et wf tournent dans la vue prouvée, ou dans une
fenêtre secondaire ou un iframe qui en tient l'accès (section 8), et le pilote de test `webfront-wf` ouvre une vue
prouvée (`TestFrontend` transmet `ViewLaunch`) : le parcours normal n'est pas touché. L'automatisation MCP (ADR-025)
garde sa propre autorité, le jeton Bearer de session, et ne passe pas par ces callbacks.

**Avec le navigateur système**, aucune vue n'est prouvée : rien ne change pour les callbacks de l'environnement, qui
restent ouverts à toute connexion admise par l'origine, et les inspecteurs restent refusés par défaut (ci-dessus). Un
client local non prouvé peut donc encore y modifier le projet ; c'est la limite connue de ce frontend, qui ne protège
aucune vue, et `trustLoopbackClients` la rend explicite. Cette garde amende la portée des permissions de
l'[ADR-016](ADR-016-bootstrap-comme-socle.md) (version 1.7).

### 3. Protocole : nœuds d'arbre sur les types existants

Aucun code de type n'est ajouté et le format binaire ne change pas. C'est l'exception de l'ADR-003 : le codec
JavaScript et `IndexFS.hpp` restent inchangés, mais les tests d'encodage et de décodage, ainsi qu'un test Jasmine dans
un vrai navigateur, démontrent la compatibilité.

Un **nœud** est un tuple de six champs, de même forme pour tous les nœuds d'une liste :

| Champ | Type C++ | Rôle |
| --- | --- | --- |
| chemin | `std::string` | identifiant opaque du nœud, construit par C++ ; le navigateur le renvoie tel quel et C++ le valide ; 64 segments et 1 024 octets au plus |
| genre | `std::string` | vocabulaire fermé : `bool`, `integer`, `real`, `string`, `enum`, `bitset`, `optional`, `variant`, `tuple`, `sequence`, `map`, `described`, `formatted`, `opaque` |
| libellé | `std::string` | nom du champ, indice ou clé formatée, tronqué à 256 octets UTF-8 sans couper un caractère |
| valeur | `std::string` | valeur formatée, tronquée à 256 octets UTF-8 sans couper un caractère |
| enfants | `double` | nombre d'enfants, exact jusqu'à 2⁵³ |
| drapeaux | `double` | bits : modifiable (1), valeur tronquée (2), retiré (4), profondeur atteinte (8), enfants modifiés (16) |

L'épique décrivait cinq champs ; le sixième porte ce que l'inspecteur doit savoir sans nouvel appel (le nœud est-il
modifiable, sa valeur est-elle complète, a-t-il disparu).

**Bornes effectives.** Chaque champ a une taille maximale en octets : le chemin 1 024, le libellé et la valeur 256,
le genre 16, plus les deux nombres et les en-têtes du codec. Un nœud encodé tient donc dans `maxNodeBytes` (2 Kio), et
une page d'un seul nœud, avec son en-tête, dans `minPageBytes` (4 Kio).

**Encodage des chemins.** Un chemin est une chaîne ASCII : la suite des segments de ses ancêtres, chacun précédé de
`/`. Chaque segment commence par une lettre qui dit sa nature, et son contenu ne contient jamais `/` :

| Segment | Forme | Taille maximale |
| --- | --- | --- |
| indice de séquence, ou rang dans un tableau associatif ou une `PagedSource` sans clé | `#` puis le décimal | 21 octets |
| champ d'un type décrit, désigné par son **rang** dans la description (le nom, qui peut être long, est le libellé) | `.` puis le décimal | 21 octets |
| clé stable d'une `PagedSource` (section 5) | `k` puis la clé encodée en pourcent : chaque octet hors de `A-Z a-z 0-9 - . _ ~` devient `%HH` | 129 octets |

La **racine** d'un observateur, l'objet que rend sa source, a le chemin vide (`""`), sans segment. L'encodage est
réversible : C++ décode un chemin reçu segment par segment, et refuse un chemin mal formé
(`webfront.inspector.unknown`). Une clé dont l'encodage dépasserait 128 octets n'est pas tronquée, ce qui la rendrait
ambiguë : son élément est désigné par sa position, comme sans clé, et n'est pas modifiable.

Un nœud n'est jamais construit avec un chemin hors limite : c'est son **parent** qui s'arrête. Quand le parent est au
64ᵉ niveau, ou quand la longueur de son chemin plus un séparateur et le plus long segment possible de ses enfants
dépasserait 1 024 octets, il est envoyé avec le drapeau « profondeur atteinte ». `children` le refuse alors
(`webfront.inspector.limit`), et l'inspecteur dit pourquoi il ne se déplie pas. Aucun chemin ne dépasse donc 1 024
octets, ni aucun nœud `maxNodeBytes`.

**Budget d'une réponse.** Une réponse doit tenir à la fois dans le tampon d'encodage et dans la taille de message :
toutes les bornes de cette décision se calculent sur `responseBudget = min(functionCallBufferSize, maxMessageSize)`, en
comptant **tous** les octets encodés du message de retour (en-tête du message et identifiant de corrélation, tuple de
la réponse, en-têtes de liste et de chaîne, nombres, booléens). Ces en-têtes ont une taille maximale fixe, connue à la
compilation (`responseHeaderBytes`). L'enregistrement des fonctions `webfront.inspector.*` vérifie, par un
`static_assert` sur les constantes de la politique, que `responseBudget` vaut au moins `minPageBytes`. Une politique
trop petite ne compile pas avec l'environnement développeur, au lieu d'échouer en cours d'inspection. La politique par
défaut (32 Kio et 64 Mio) donne un budget de 32 Kio, soit seize pages minimales.

**Budget d'une requête.** C++ ferme une connexion qui annonce un message plus grand que `maxMessageSize` (ADR-003) :
une requête doit donc tenir dans `requestBudget = maxMessageSize`, **tous ses arguments encodés compris**. Seule
l'écriture porte un argument dont la taille dépend de l'utilisateur, le texte saisi ; tous ses autres arguments sont
bornés : l'identifiant de l'observateur et la révision (des nombres), le
chemin (1 024 octets), la valeur attendue (256 octets, puisqu'une valeur tronquée n'est pas modifiable), le nom de la
fonction et les en-têtes du message d'appel. Leur taille maximale encodée est une constante
(`writeRequestOverheadBytes`), et le texte fait au plus `maxWriteTextBytes = requestBudget - writeRequestOverheadBytes`
octets. Le `static_assert` de l'enregistrement vérifie aussi que `maxWriteTextBytes` vaut au moins 1 Kio.

La borne est vérifiée des deux côtés. `webfront.inspector.limits()` donne à l'inspecteur `maxWriteTextBytes`, et il
mesure le texte en octets UTF-8 avant l'envoi, en tenant compte aussi de la limite de son propre codec
(`maxMessageBytes`) : un texte trop long est refusé dans la page, avec le code `webfront.inspector.tooLarge`, sans rien
envoyer. C++ vérifie à nouveau la longueur du texte décodé et refuse avec le même code ; ce contrôle couvre un client
qui n'aurait pas respecté la borne sans dépasser `maxMessageSize`. Les autres requêtes ont toutes leurs arguments
bornés et tiennent dans `minPageBytes`.

Une **page** est un tuple de premier niveau `(liste de nœuds, suivant, révision, resynchroniser)`, où `suivant` est la
position ou le curseur de l'appel suivant (0 à la fin) et `resynchroniser` un booléen. L'ADR-003 permet une liste dans
un tuple de premier niveau, pas de liste imbriquée : l'arbre circule donc à plat, un niveau déplié par appel. **Toute**
réponse qui porte des nœuds est une page : aucun appel ne rend une liste dont la longueur dépend de l'état observé. Les
appels sont :

- `webfront.inspector.watches(révision, après, nombre)` : la découverte des observateurs (« Découverte », ci-dessous) ;
- `webfront.inspector.children(observateur, chemin, révision, suivant, nombre)` : une page des enfants d'un nœud
  (« Cohérence des listes d'enfants », ci-dessous).
- `webfront.inspector.changes(observateur, révision, curseur, nombre)` : une page des différences (ci-dessous).
- `webfront.inspector.value(observateur, chemin, instantané, décalage)` : un fragment de la valeur complète d'un
  nœud tronqué (ci-dessous) ;
- `webfront.inspector.fold(observateur, chemin, révision)` : retire de la mémoire un nœud replié et ses descendants,
  et rend la nouvelle révision. Comme tout appel qui modifie la mémoire, il porte la révision que l'inspecteur détient,
  validée avant toute interruption de passe (« Révision périmée », ci-dessous) ;
- `webfront.inspector.reset(observateur)` : vide la mémoire de l'observateur pour cette connexion (ci-dessous) ;
- `webfront.inspector.limits()` : les bornes de cette section, dont `maxWriteTextBytes`.

Le nombre est un argument de chaque appel, puisque le pont n'a pas d'argument facultatif : l'inspecteur demande 200
par défaut, et C++ le ramène à 1 000 au plus (0 ne demande que la révision, pour `watches`). C++ réduit chaque page pour qu'elle tienne dans `responseBudget` et
rend le `suivant` qui reprend là où elle s'arrête : aucun appel ne lève `BufferOverflowException` ni ne dépasse
`maxMessageSize`, quel que soit l'état observé. Les seuls échecs d'un appel valide sont les refus typés de cette
décision (`webfront.inspector.*`).

**Valeur complète.** Une valeur formatée peut dépasser `maxMessageSize` : `value` est donc paginé par octets, sur un
instantané désigné par un identifiant.

- L'appel avec l'instantané 0 (et le décalage 0) formate la valeur en une seule opération (section 5), la range comme
  instantané de la connexion et lui donne un **identifiant** : un compteur de la connexion, jamais réutilisé tant
  qu'elle vit. Il rend `(fragment, taille totale, décalage suivant, plafonnée, instantané)`.
- Les appels suivants donnent cet identifiant et le décalage suivant reçu. C++ ne sert un fragment que si l'identifiant
  est celui de l'instantané courant, et l'observateur, le chemin et le décalage ceux de cet instantané ; sinon il
  refuse avec `webfront.inspector.stale`, et l'inspecteur recommence avec l'instantané 0. Une seconde lecture qui
  remplace l'instantané fait donc échouer explicitement la suite de la première ; elle ne peut jamais lui servir un
  fragment d'une autre valeur.
- L'instantané reste cohérent même si l'objet change entre deux fragments. Un fragment fait au plus
  `responseBudget - responseHeaderBytes` octets et ne coupe jamais un caractère UTF-8.
- L'instantané est plafonné à 16 Mio : au-delà, la valeur est coupée et `plafonnée` est vrai, ce que l'inspecteur
  affiche. Une connexion a au plus un instantané, libéré à la déconnexion.

**Formatage borné.** Aucune valeur n'est formatée en entier puis tronquée par WebFront. Toute valeur, celle d'un nœud
(256 octets) comme celle d'un instantané (16 Mio), s'écrit dans un **récepteur borné** : une chaîne n'est copiée que
jusqu'à la borne, et un type formatable passe par `std::format_to_n` vers un tampon de la taille de la borne, qui compte
la taille qu'aurait la sortie complète sans la stocker. La troncature est détectée par cette taille, et le drapeau
« valeur tronquée » ou `plafonnée` la signale.

La garantie porte sur **les allocations de WebFront**, et sa portée dépend du genre (section 4) :

- pour les genres que WebFront rend lui-même (scalaires, chaînes, énumérations décrites, `bitset`, conteneurs,
  `optional`, `variant`, tuples, types décrits), la mémoire d'une valeur est bornée par la borne plus une constante, et
  aucun code de l'application n'est appelé, hors de la source et d'une description ;
- pour le genre `formatted`, WebFront appelle le `std::formatter` du type. Il borne ce qu'il stocke de la sortie, pas ce
  que le formateur alloue pour la produire : un formateur qui construit une chaîne intermédiaire, ou un sous-formatage
  par `std::format`, alloue à sa guise. Sa mémoire comme son temps sont **sous la responsabilité de l'application**, et
  ils courent sous le verrou de la source. La documentation le dit ; l'application qui observe un type au formatage
  coûteux le décrit (`forms::Description`) ou le pagine (`PagedSource`) plutôt que de compter sur `std::formatter`.

WebFront ne restreint pas les formateurs acceptés : il ne peut pas savoir ce qu'un formateur alloue, et refuser
`std::formatter` priverait l'inspecteur de tous les types de l'application qui n'ont pas de description.

**Découverte.** Dans tous les appels, `observateur` désigne l'**identifiant** d'un observateur, un nombre que WebFront
attribue à chaque `watch` dans l'ordre d'enregistrement et ne réutilise jamais tant que l'application vit ; le nom
n'est qu'un libellé. Un observateur retiré puis réenregistré sous le même nom reçoit donc un nouvel identifiant, et
aucun appel destiné à l'ancien ne peut atteindre le nouveau.

Le registre des observateurs a sa propre **révision**, incrémentée à chaque enregistrement et à chaque retrait.
`watches(révision, après, nombre)` rend une page `(liste de descripteurs, suivant, révision du registre, changé)`. Un
descripteur est le tuple `(identifiant, nom, genre de la racine, fichier:ligne du watch, modifiable)`, avec le nom et
l'emplacement tronqués à 256 octets UTF-8 : sa taille est bornée, et la page suit les règles de budget ci-dessus. La
page liste les observateurs vivants d'identifiant supérieur à `après`, dans l'ordre des identifiants, et `suivant` est
le dernier identifiant servi (0 à la fin). Ce curseur par identifiant est stable : un retrait ne décale rien, et un
enregistrement arrive en fin de liste ; aucun observateur vivant pendant toute la liste n'est sauté ni servi deux fois.
`changé` est vrai quand la révision fournie n'est plus celle du registre.

La découverte ne lit aucun objet observé et n'appelle aucune source : elle coûte une lecture de la table. L'inspecteur
l'appelle à chaque période avec `nombre` à 0, ce qui rend seulement la révision du registre et `changé` ; quand le
registre a changé, il relit la liste depuis `après` = 0, ajoute les nouveaux observateurs, et retire ceux qui ont
disparu avec leurs arbres. Un appel qui vise un observateur retiré, entre-temps, reçoit `webfront.inspector.unknown`
(« Réponse terminale », section 5).

**Mémoire par connexion.** C++ garde, pour chaque connexion et chaque observateur, la valeur affichée des nœuds envoyés,
comme le fait `forms::inspector`, et la révision de cette mémoire. Elle compte 10 000 nœuds au plus : au-delà,
`children` échoue avec `webfront.inspector.limit` et l'inspecteur propose de replier des nœuds. Elle est libérée à la
déconnexion, et `fold` en retire un nœud et ses descendants.

**Ordre et révision.** Les appels d'une connexion sur un observateur sont traités un à un, dans l'ordre de réception,
même quand ils s'exécutent sur un exécuteur ; deux Promises en vol ne se croisent donc pas. Tout appel qui modifie la
mémoire (`children`, `fold`, `write`, et `changes` au curseur 0) lui donne une nouvelle révision, la rend dans sa
réponse, et l'inspecteur l'adopte. L'inspecteur n'envoie donc un appel qui modifie la mémoire d'un observateur
qu'après la réponse du précédent : envoyés à la suite sans attendre, le second porterait une révision que le premier
vient de rendre périmée. Les lectures qui ne modifient pas la mémoire (`value`, `limits`, `containers`) peuvent être
envoyées à tout moment.

**Révision périmée.** Une révision qui n'est pas celle de la mémoire signifie que la vue de l'inspecteur et la mémoire
ont divergé : l'inspecteur a manqué une réponse, ou n'a pas respecté l'ordre ci-dessus. Le contrat est unique pour
tous les appels qui portent une révision, et il est vérifié **avant** tout autre effet :

- `children`, `fold` et `changes` répondent par une resynchronisation : la mémoire de l'observateur pour cette
  connexion est vidée, une passe en cours est **libérée** avec son ensemble rangé, sans restauration ni interruption
  ordinaire, et rien n'est déplié ni replié ;
- `write`, qui ne rend pas de page, est refusé avec `webfront.inspector.stale` **sans aucun effet** : ni la mémoire,
  ni une passe en cours, ni l'objet ne changent. L'appel suivant de l'inspecteur, qui porte la même révision périmée,
  reçoit alors la resynchronisation.

Une révision périmée ne préserve donc jamais une passe en cours, sauf pour `write`, qui ne touche à rien ; et une
resynchronisation libère toujours la passe.

**Atomicité des refus.** Règle générale : **une Promise rejetée garantit que ni la mémoire, ni une passe en cours, ni
l'objet n'ont changé**, et **un appel qui a changé la mémoire se résout toujours et porte la révision qui en résulte**.
Un appel qui modifie la mémoire s'exécute donc en deux phases :

1. **validation, sans effet.** Tout ce qui peut échouer sans lire l'objet est vérifié d'abord : accès, observateur
   connu, chemin bien formé et présent dans la mémoire (sauf la racine, ci-dessous), révision (contrat ci-dessus), drapeaux du nœud (modifiable,
   profondeur atteinte), taille et analyse du texte d'une écriture (le genre et les bornes viennent du type), place
   dans la mémoire, calculée sur la mémoire telle qu'elle serait après une interruption, puisque l'ensemble rangé la
   donne. La source est ensuite appelée ; si elle échoue avant d'entrer dans le visiteur (exception, visiteur non
   appelé, accès qui lève), rien n'a encore changé. Un échec de cette phase **rejette** la Promise ;
2. **effet, sans rejet.** Une fois dans la fenêtre d'accès, l'interruption éventuelle de la passe et l'opération
   s'exécutent, et l'appel **se résout** quoi qu'il arrive ensuite. Les échecs qui ne se voient qu'en lisant l'objet,
   ou qui viennent du code de l'application, sont rapportés dans la réponse :
   - pour `write`, la réponse est un tuple `(révision, code, message)` : code vide si l'écriture est appliquée, sinon
     `webfront.inspector.stale` (précondition fausse) ou `webfront.inspector.refused` (refus du mutateur, avec le
     message de son exception, borné comme ci-dessous) ; dans les deux cas l'objet est inchangé, mais la révision est celle de la mémoire
     après l'interruption, et l'inspecteur l'adopte ;
   - pour `children`, `fold` et `changes`, la page ne peut plus échouer : une exception d'un formateur devient la
     valeur du nœud (`‹exception : message›`, genre `opaque`), et une exception de `key` ou de `find` fait traiter
     l'élément comme sans clé (position, non modifiable). Une exception de la source après le retour du visiteur est
     écrite dans les diagnostics et ignorée.

**Messages bornés.** Tout texte qui vient du code de l'application et entre dans une réponse ou un refus (message de
l'exception d'un mutateur, d'un formateur, de `key`, de `find` ou de la source) est tronqué à **256 octets UTF-8** sans
couper un caractère multioctet (on recule jusqu'au début du dernier caractère complet), et ses caractères de contrôle
sont rendus visibles (section 4) ; un message tronqué se termine par `…`. Le code est l'un des codes fixes de cette
décision. La réponse de `write` a donc une taille maximale fixe, comptée dans `responseHeaderBytes`, et tient toujours
dans `responseBudget` ; la révision ne peut pas être perdue faute de place. Les refus qui rejettent une Promise portent
le même message borné, dans le code `exception` de l'ADR-003.

Un refus d'écriture qui suit une interruption ne fait donc jamais perdre la révision, et ne provoque pas de
resynchronisation : la promesse qu'une passe interrompue n'en demande pas tient aussi en cas d'échec.

**Différences paginées.** Une passe de `changes` commence au curseur 0, avec la révision R que l'inspecteur détient :

1. si R est la révision de la mémoire, C++ compare la mémoire à l'état observé, en une seule lecture (section 5), et
   range l'ensemble des différences : nœuds modifiés et nœuds retirés, **chacun avec sa valeur d'avant la passe**. La
   mémoire prend la nouvelle valeur, sous une révision R′, et la première page de l'ensemble est rendue avec R′ ;
2. l'inspecteur demande les pages suivantes avec (R′, curseur) jusqu'à un `suivant` nul. Elles sont lues dans
   l'ensemble rangé, pas dans l'état courant : la passe est cohérente même si l'objet change entre deux pages. Après la
   dernière, l'inspecteur détient R′ ;
3. une révision ou un curseur qui ne correspondent pas à la passe en cours (page perdue, passe abandonnée, mémoire
   libérée) donnent une page vide avec `resynchroniser` vrai (voir ci-dessous).

**Resynchronisation.** Une réponse avec `resynchroniser` vrai a déjà **vidé** la mémoire de cet observateur pour cette
connexion, et libéré la passe et son ensemble rangé ; elle porte la révision de la mémoire vide. L'inspecteur jette
alors tout l'arbre affiché de l'observateur, en ne gardant que la liste des chemins qu'il avait dépliés, puis relit
depuis la racine par `children` avec la révision rendue (la racine est toujours dépliable, section 3), et ne redéplie que les chemins qui figurent encore dans les pages reçues. Un nœud
supprimé dont la suppression n'était annoncée que dans une page perdue n'est renvoyé par aucun `children` : il n'est
donc ni affiché ni gardé en mémoire, et n'occupe rien de la limite de 10 000 nœuds. L'inspecteur peut aussi demander
cette remise à zéro lui-même avec `webfront.inspector.reset(observateur)`, qui rend la révision de la mémoire vide.

L'ensemble rangé est borné par la mémoire (10 000 nœuds) et libéré au début de la passe suivante ou à la déconnexion.

**Durée d'une passe.** Le nombre de pages d'une passe n'est pas borné par une constante : il dépend du nombre de
différences (10 000 au plus) et de la taille des nœuds rapportée à `responseBudget`. Avec le budget minimal, une page ne
porte parfois qu'un nœud, et une passe peut demander des milliers d'allers-retours. Rien n'attend donc la fin d'une
passe : toute passe peut être **interrompue** à tout moment, sans resynchronisation.

**Interruption.** Une passe en cours est interrompue par un appel qui modifie la mémoire (`children` qui déplie, `fold`
qui replie, `write`), ou par un nouvel appel `changes` au curseur 0 avec la révision de la passe en cours, que
l'inspecteur fait quand il veut repartir de l'état courant.

La **révision** que porte l'appel interrompant est validée **avant** l'interruption : elle doit être celle de la mémoire
à cet instant, c'est-à-dire R′, la révision de la passe en cours, que l'inspecteur détient depuis la première page.
Une révision plus ancienne suit le contrat « Révision périmée » : resynchronisation, qui libère la passe, pour
`children`, `fold` et `changes` ; refus sans effet, qui laisse la passe en cours, pour `write`. Une fois validée, l'appel ne compare plus de révision : une écriture vérifie le reste de sa
précondition (section 6) contre la mémoire restaurée, qui est exactement ce que l'inspecteur a reçu. Une écriture qui
vise un nœud d'une page non servie est donc comparée à l'ancienne valeur, celle que l'utilisateur voit : si l'objet a
changé depuis, elle est refusée comme périmée, et c'est le résultat attendu.

Avant d'exécuter l'appel, C++ rend aux entrées des pages
**non encore servies** leur valeur d'avant la passe, gardée dans l'ensemble rangé : un nœud modifié reprend son ancienne
valeur dans la mémoire, et un nœud retiré y revient. La mémoire décrit alors exactement ce que l'inspecteur a reçu, ni
plus ni moins, sous une nouvelle révision que la réponse de l'appel rend toujours, même pour une écriture refusée
(« Atomicité des refus »). Puis l'appel s'exécute : un `children` qui relit un nœud
de la partie non servie y met sa valeur fraîche, et un `fold` retire aussi les entrées restaurées sous le nœud replié.
Les différences non servies ne sont pas perdues : la passe suivante les retrouve en comparant la mémoire restaurée à
l'objet.

L'inspecteur n'attend donc pas la fin d'une passe : un dépliement, un repli ou une écriture partent dès la réponse à
l'appel précédent qui modifiait la mémoire, et la passe interrompue reprend à la période suivante. La resynchronisation (mémoire vidée) ne sert plus qu'aux incohérences
réelles : une révision ou un curseur qui ne correspondent à rien de ce que C++ a servi.

L'inspecteur interroge à une période choisie, 250 ms par défaut, et peut se mettre en pause. C'est une lecture à
l'initiative du navigateur (pull) : C++ n'envoie rien sans demande, et un inspecteur fermé ne coûte rien.

Le chemin d'un élément de séquence est son indice. Celui d'un élément de tableau associatif est son rang dans
l'itération, et son libellé est la clé formatée. Une `PagedSource` peut fournir une clé stable par élément,
qui remplace alors le rang dans le chemin (section 5). Quand le conteneur change, un chemin par indice ou par
rang peut désigner un autre élément ou plus aucun : C++ le revalide à chaque lecture et, quand il ne résout plus,
signale le nœud comme retiré. En lecture, l'identité d'un nœud au fil des modifications n'est pas garantie ; une
écriture, elle, n'est appliquée que sous la précondition de la section 6.

**Cohérence des listes d'enfants.** Les pages de `children` d'un même parent sont lues à des moments différents, et le
conteneur peut changer entre deux pages : une insertion en tête décale tout, et la page suivante relirait un élément
déjà reçu en sautant le dernier. Trois règles empêchent qu'un élément soit perdu durablement :

1. **Le parent voyage avec ses enfants.** Le premier nœud de chaque page de `children` est le parent lui-même, lu dans
   la même opération que la page : son nombre d'enfants et sa valeur sont ceux du moment de la page, et la mémoire les
   enregistre. La mémoire garde aussi, pour chaque parent déplié, le chemin de l'enfant à chaque position chargée.

**La racine est toujours dépliable.** La racine (chemin vide) est le seul chemin qu'un appel peut viser sans qu'il soit
dans la mémoire : elle existe tant que l'observateur existe. `children(observateur, "", révision, 0, nombre)` est donc
admis avec une mémoire vide, que ce soit à la première ouverture d'un observateur découvert par `watches`, après
`reset` ou après une resynchronisation, dont la révision rendue est celle que cet appel doit porter. Sa page commence,
comme toute page, par le parent, ici la racine, qui entre alors dans la mémoire ; ses enfants suivent. Une racine sans
enfants (un scalaire observé) donne une page qui ne contient qu'elle : c'est ainsi que l'inspecteur lit sa valeur, et
qu'elle devient modifiable si l'observateur l'est. Les autres appels (`fold`, `value`, `write`) visent un nœud de la
mémoire, racine comprise, et exigent donc d'abord ce premier `children`. `fold` de la racine retire tous ses
descendants mais garde la racine.
2. **Une liste se lit sur un état cohérent, ou recommence.** `suivant` reste la position suivante ; C++ garde dans la
   mémoire, pour la liste en cours de chaque parent, l'état du parent à la dernière page servie : son nombre d'enfants
   et, pour une `PagedSource` à clés, la clé du dernier enfant servi. Chaque page suivante vérifie, dans son opération, que le parent n'a pas changé de nombre
   d'enfants et, avec des clés, que `find` de la dernière clé servie rend toujours la position attendue. Sinon la page
   ne sert rien de plus : elle rend le parent avec le drapeau « enfants modifiés » et un `suivant` nul, la mémoire
   retire les enfants de ce parent, et l'inspecteur recommence la liste à la position 0. Une page n'est donc jamais
   lue sur un autre état que celui des précédentes, au moins pour ce que le nombre d'enfants et les clés révèlent.
3. **Une passe vérifie la structure des parents dépliés.** Pour chaque parent déplié, `changes` compare son nombre
   d'enfants et, avec des clés, la clé à chaque position chargée (un appel `key` par position, borné par la mémoire)
   à ce que la mémoire a enregistré. Une différence rapporte le parent avec « enfants modifiés » ; l'inspecteur relit
   alors ses positions chargées par `children`. Sans clé, une modification qui garde le nombre d'enfants (insertion et
   suppression simultanées, réordonnancement) apparaît comme des changements de valeur aux positions chargées, que la
   passe corrige position par position.

Une position chargée est ainsi corrigée par la passe suivante, une position non chargée est signalée par le nombre
d'enfants du parent, que l'inspecteur charge quand elle entre dans sa fenêtre, et une liste interrompue par un
changement recommence. Dans l'exemple d'une liste `[a, b, c, d]` dont les deux premiers éléments ont été lus avant une
insertion en tête, la page suivante voit cinq enfants au lieu de quatre, ne sert rien et fait recommencer la liste, qui
lit alors `x, a, b, c, d`. La garantie reste positionnelle sans clé : entre deux passes, l'inspecteur peut montrer un
état intermédiaire, jamais l'oublier.

### 4. Rendu des valeurs

Le genre et la valeur d'un nœud sont choisis dans cet ordre :

1. `bool`, entiers, flottants, énumérations décrites, chaînes (`std::string`, `std::string_view`), `std::bitset<N>` ;
2. `std::optional`, `std::variant`, `std::pair` et `std::tuple`, dont les enfants sont la valeur contenue ou les
   membres ;
3. tout `std::ranges::sized_range` (`sequence`) et les tableaux associatifs (`map`), dont les enfants sont les
   éléments ;
4. un type qui a une `forms::Description` (C++23) ou une description par réflexion (C++26, GCC 16, `Reflection.hpp`) :
   genre `described`, un enfant par champ ;
5. un type formatable par `std::formatter` : genre `formatted`, sans enfants ;
6. sinon le nom du type et le genre `opaque`.

Les caractères de contrôle d'une valeur sont rendus visibles avant l'affichage, comme dans les commandes de fichiers du
terminal (ADR-023) : une chaîne de l'application n'est pas de confiance pour le terminal ni pour la page.

### 5. Observateurs et threads

```cpp
// Objet partagé : tout le travail de l'observateur se fait dans visit, sous le verrou de l'application.
auto watch = app.watch("grid", [&](webfront::inspect::Visit<const Grid> visit) {
    std::scoped_lock lock{mutex};
    visit(grid);
});
// Objet confiné à un thread : l'accès rend une référence, et tout le travail se fait sur l'exécuteur de ce thread.
auto other = app.watch("generations", [&] -> const History& { return history; }, app.uiExecutor());
```

`watch(nom, source [, exécuteur] [, écriture])` rend une poignée RAII, comme `UISubscription` : l'observateur existe
tant qu'elle vit, et la poignée reste sûre après la destruction de l'application. Contrairement à `UISubscription`, sa
destruction peut attendre une opération en cours (« Destruction de la poignée », ci-dessous).

Un observateur ne lit jamais un objet que possède un autre thread sans synchronisation. Une **opération** de
l'observateur (une page de `children`, le calcul d'une passe de `changes`, `value`, `write`) se fait entièrement dans
une seule fenêtre d'accès que l'application définit ; aucune référence à l'objet n'en sort. L'application choisit la
source :

- **un visiteur** (`Visit<const T>`, ou `Visit<T>` pour écrire) : WebFront appelle la source avec un visiteur, et la
  source l'appelle une fois avec l'objet, sous son propre verrou. L'opération entière s'exécute pendant cet appel, donc
  sous le verrou. C'est la forme d'un objet partagé entre threads. Une source qui n'appelle pas le visiteur fait
  échouer l'opération avec `webfront.inspector.unavailable` ;
- **une référence sur un exécuteur** : l'objet n'est touché que par le thread d'un exécuteur (`app.uiExecutor()`, la
  boucle d'un `host-loop`, un exécuteur de l'application). L'accès et l'opération entière s'exécutent sur cet
  exécuteur, et la référence reste valide pendant toute l'opération ;
- **une valeur** : l'accès rend une copie, que l'observateur parcourt librement. C'est sûr partout, mais l'objet
  entier est copié à chaque opération ; cette forme convient aux petits objets et ne bénéficie d'aucune des bornes de
  coût ci-dessous.

Avec un exécuteur, l'opération est une `webfront::ResultTask` : la Promise se règle quand elle se termine, la
déconnexion du navigateur l'arrête, et l'arrêt la joint (ADR-004, ADR-006). Sans exécuteur, la source est appelée sur
le thread du serveur, celui des callbacks `cppFunction`. Un accès qui rend une référence sans exécuteur n'est correct
que si l'objet n'est touché que par ce thread ; la documentation le dit en ces termes et l'exemple montre le visiteur
sous verrou. Un verrou pris dans un accès qui rend une référence ne protège rien, puisqu'il est relâché avant le
parcours : c'est l'erreur que la forme du visiteur évite.

**Coût d'une opération.** Avec un visiteur ou une référence, aucune opération ne copie l'objet observé, et la mémoire
ainsi que la taille de message d'une opération sont proportionnelles à la page, non au conteneur. Le temps dépend du
conteneur :

- une `std::ranges::random_access_range` atteint la position demandée en temps constant : une page coûte sa taille ;
- les autres `sized_range` et les tableaux associatifs n'ont pas d'accès par rang : atteindre la position p parcourt
  les p éléments précédents. Une passe de `changes` regroupe les nœuds par parent et parcourt chaque parent déplié une
  seule fois, jusqu'au plus grand rang affiché ;
- un type qui modèle `webfront::inspect::PagedSource` (`size()`, `visit(premier, nombre, fonction)` et, en option,
  `key(position)` et `find(clé)`) fournit lui-même l'accès par page : une page coûte ce que l'application en décide.
  C'est la voie d'un très grand conteneur sans accès aléatoire, ou d'un contenu calculé à la demande.

**Clés d'une `PagedSource`.** Une `PagedSource` qui fournit `key(position)` fournit aussi `find(clé)`, qui rend la
position actuelle de l'élément ou rien. Son contrat est celui d'une clé primaire : à un instant donné, une clé désigne
au plus un élément (**unicité**), et un élément garde sa clé tant qu'il existe (**stabilité**). WebFront ne peut pas
vérifier ce contrat sur tout le conteneur sans le parcourir ; il en vérifie ce qui est local et borné :

- un chemin par clé se résout par `find`, puis C++ vérifie que `key` à la position trouvée rend bien la même clé ;
  sinon le nœud est retiré ;
- deux éléments d'une même page qui rendent la même clé violent l'unicité : ils sont désignés par leur position, ne
  sont pas modifiables, et un diagnostic nomme l'observateur une fois.

Une clé dupliquée entre deux pages, ou une clé qui change d'élément, échappe à ces contrôles. La précondition
d'écriture (section 6) compare alors encore le libellé, la valeur et les ancêtres affichés : une source qui viole son
contrat de clé retombe sur la garantie d'un conteneur sans clé, jamais en dessous.

Une page tardive d'une très grande liste chaînée ou d'une `std::map` peut donc coûter un parcours linéaire, sous le
verrou de l'application ; la documentation le dit, et `PagedSource` en est le remède.

**Destruction de la poignée.** Une source capture en général des références à des objets de l'application (l'objet
observé, son mutex). Détruire la poignée doit garantir qu'aucune opération ne les touche ensuite ; deux contrats
existent, selon ce que la source capture.

- **Source à références (par défaut) : la destruction attend.** Elle retire l'observateur de la table, aucune opération
  ne commence ensuite, et elle attend la fin d'une opération déjà commencée sur un autre thread. C'est le contrat d'un
  `std::jthread` qu'on joint, et il en a la règle : **la poignée se détruit hors de tout verrou que la source prend**,
  et hors de l'exécuteur de l'observateur quand l'opération en cours y attend. Sinon l'opération attend le verrou que
  tient le thread qui attend l'opération : c'est un interblocage, et WebFront ne peut pas le lever, faute de connaître
  les verrous de l'application. L'exemple et la documentation détruisent la poignée avant de prendre le verrou, ou
  par un membre déclaré après le mutex (détruit avant lui, hors section critique).
- **Source propriétaire : la destruction n'attend pas.** `watch(nom, propriétaire, source [, exécuteur])` reçoit un
  `std::shared_ptr` vers l'état que la source utilise (l'objet et son mutex, typiquement), et la source ne capture rien
  d'autre par référence. Chaque opération garde une copie du `shared_ptr` le temps de son exécution. La destruction de
  la poignée retire l'observateur et rend la main **sans attendre** : une opération en cours se termine sur un état qui
  vit encore, et sa réponse est remplacée par le refus terminal ci-dessous. Détruire la poignée sous le verrou de la
  source est alors sans danger.
  C'est la forme à choisir quand l'application ne peut pas garantir l'ordre de destruction.

**Destruction depuis l'opération elle-même** (dans la source, le visiteur ou le mutateur, sur le thread qui exécute
l'opération) :

- **source propriétaire : permise.** L'opération garde le `shared_ptr` de l'état jusqu'à sa fin : l'objet et son mutex
  vivent encore quand elle reprend, même si la poignée était leur dernier autre propriétaire. L'opération se termine et
  sa réponse est remplacée par le refus terminal ci-dessous ;
- **source à références : interdite.** Attendre l'opération serait s'attendre soi-même, et ne pas attendre la laisserait
  reprendre sur des objets que l'application peut détruire juste après la poignée. Le destructeur reconnaît ce cas (il
  compare son thread à celui de l'opération en cours, enregistré au début de celle-ci), écrit un diagnostic qui nomme
  l'observateur et le fichier et la ligne de son `watch`, puis appelle `std::terminate`. C'est une erreur de
  programmation qui ne peut pas être rendue sûre ; l'arrêt immédiat, dans une construction de développement, vaut mieux
  qu'une corruption silencieuse. Une application qui doit retirer un observateur depuis sa source utilise une source
  propriétaire, ou poste la destruction sur un autre thread.

**Réponse terminale.** Tant que la connexion vit, **toute requête reçue reçoit une réponse**, résolue ou rejetée ;
aucune Promise ne reste en attente parce qu'un observateur a disparu. Le retrait d'un observateur libère sa mémoire,
ses passes et ses instantanés pour toutes les connexions, puis :

- une opération déjà commencée se termine (après l'attente du destructeur pour une source à références, sur l'état
  qu'elle garde pour une source propriétaire), et sa réponse est **remplacée** par le refus
  `webfront.inspector.unknown` : son résultat porterait une révision d'une mémoire qui n'existe plus ;
- une opération en file sur l'exécuteur, pas encore commencée, est refusée de même sans appeler la source ;
- tout appel ultérieur qui vise cet identifiant est refusé de même.

Ce refus est la seule exception à l'atomicité de la section 3, et elle est explicite : la mémoire n'existe plus, donc
aucune révision n'est perdue, mais une écriture déjà appliquée quand l'observateur est retiré a bien modifié l'objet.
Son refus porte alors le message « observateur retiré après l'écriture », pour que l'inspecteur sache que la valeur a
été posée ; dans tous les autres cas, le refus `unknown` garantit que l'objet n'a pas été modifié. À la déconnexion, il n'y a plus personne à qui répondre ; à l'arrêt de l'application, les tâches sont arrêtées
et jointes (ADR-006) et les connexions fermées, ce qui rejette les Promises en attente côté page (ADR-004).

Comme l'interblocage du premier contrat est silencieux, une destruction qui attend plus de cinq secondes écrit un
diagnostic (ADR-011) qui nomme l'observateur et le fichier et la ligne de son `watch` (section 7), puis continue
d'attendre : le diagnostic désigne la cause, il ne change pas le contrat.

Le nom d'un observateur est unique tant que sa poignée vit et fait 256 octets au plus ; un second `watch` du même nom,
ou un nom plus long, lève `std::invalid_argument`. Les appels désignent l'observateur par son identifiant
(« Découverte », section 3).

### 6. Modification à la demande de l'application

Un observateur est en lecture seule par défaut. L'application rend des valeurs modifiables en fournissant l'écriture :
un visiteur non constant (`Visit<T>`) ou une référence non constante sur un exécuteur, éventuellement avec un
**mutateur** `mutate(T&, chemin, valeur)` qui applique le changement à sa façon et peut le refuser pour garder ses
invariants. L'écriture est une opération comme les autres (section 5) : la vérification de la précondition, le mutateur
et l'affectation s'exécutent dans **la même** fenêtre d'accès, pendant l'appel du visiteur, sous le verrou de
l'application, ou sur l'exécuteur. Aucune autre modification ne peut donc s'intercaler entre la vérification et
l'écriture. Une source qui rend une valeur ne peut pas écrire : sa copie n'est pas l'objet, et vérifier sur une copie
puis écrire ailleurs laisserait cet intervalle ouvert.

`webfront.inspector.write(observateur, révision, chemin, attendu, texte)`, où `révision` est celle que l'inspecteur
détient et `attendu` la valeur formatée qu'il affichait pour ce nœud, rend `(révision, code, message)` selon la règle
« Atomicité des refus » (section 3). Il :

1. vérifie, sans effet, l'accès (section 2), la révision, la taille du texte et que le nœud porte le drapeau
   « modifiable » ;
2. analyse le texte selon le genre, toujours sans effet : booléens, entiers et flottants (dans les bornes du type, et
   dans celles d'un champ de `forms::Description` quand il en a), chaînes, énumérations décrites, `std::bitset<N>`
   (N chiffres binaires). Les autres genres ne sont pas modifiables ;
3. dans la fenêtre d'accès, interrompt une passe en cours, puis vérifie la **précondition** : le nœud cible ainsi
   que **chacun de ses ancêtres** montrent encore, dans l'objet, exactement ce que la mémoire a enregistré pour eux
   (libellé, valeur formatée, nombre d'enfants), la valeur de la cible étant aussi égale à `attendu`. Une insertion ou
   une suppression avant la cible change le nombre d'enfants d'un ancêtre ; un remplacement change la valeur ou le
   libellé de la cible. Dans ces cas, l'écriture n'est pas appliquée : la réponse porte le code
   `webfront.inspector.stale` et la révision courante, et l'inspecteur relit le nœud avant de la proposer à nouveau ;
4. construit la nouvelle valeur dans un temporaire, puis l'applique par le mutateur ou par affectation, toujours dans
   la même fenêtre.

La précondition compare ce que l'utilisateur a vu à ce que l'objet montre, comme un « compare-and-swap » sur l'état
affiché. Elle ne peut pas distinguer deux éléments sans clé dont tout ce qui est affiché est identique : un déplacement
qui laisse inchangés le libellé et la valeur de la cible, et le nombre d'enfants de chaque ancêtre, n'est pas détecté.
L'écriture pose alors la valeur saisie sur un élément identique en tout point visible. Un nœud dont le libellé ou la
valeur affichés sont tronqués n'est pas modifiable, puisque la comparaison porterait sur un préfixe. Une application
qui a besoin d'une identité exacte fournit une `PagedSource` avec des clés : le chemin porte alors la clé, que `find`
résout et que la précondition vérifie, sous le contrat d'unicité et de stabilité de la section 5.

Une révision périmée ou une analyse refusée (`webfront.inspector.stale`, `webfront.inspector.parse`), un nœud non
modifiable (`webfront.inspector.readOnly`) ou un texte trop long (`webfront.inspector.tooLarge`) **rejettent** la
Promise : ils sont vus pendant la validation, avant tout effet. Une précondition fausse (`webfront.inspector.stale`) ou
un refus du mutateur (`webfront.inspector.refused`, avec le message de son exception) ne se voient qu'après une
éventuelle interruption : la Promise **se résout** avec ce code et la révision qui en résulte. Dans tous les cas l'objet
est inchangé et l'inspecteur affiche la raison. Un mutateur fourni par l'application doit être tout ou rien ; c'est son
contrat.

### 7. Fichier et ligne d'enregistrement

`cppFunction` et `watch` prennent un dernier paramètre `std::source_location location = std::source_location::current()`.
L'inspecteur affiche le fichier et la ligne de l'enregistrement. Les appels existants compilent sans modification, y
compris la forme explicite `cppFunction<R, Args...>(nom, f)` ; le changement est visible seulement pour du code qui
nommerait la signature exacte du membre.

Le registre de chaque `cppFunction` (#681) garde le nom, la signature écrite en TypeScript comme les déclarations
générées, le nombre d'appels et d'échecs, la dernière erreur et la durée moyenne. Les compteurs sont des incréments
atomiques relâchés ; la durée n'est mesurée que pendant qu'un inspecteur est ouvert, pour qu'une application sans
inspecteur n'en paie pas le coût.

**Ce qui est enregistré.** À chaque `cppFunction` (de l'application ou d'un espace, `Space::cppFunction`), WebFront
garde un enregistrement : le nom, la signature, le fichier et la ligne, le chemin de l'espace propriétaire (vide pour
l'application), si le premier paramètre est l'appelant (`UI`), et si la fonction rend un émetteur (`ResultTask`), donc
répond plus tard. L'enregistrement appartient au gestionnaire de la fonction : un enregistrement refusé (nom déjà pris)
ou une fonction d'espace libérée avec son espace disparaît du registre sans autre retrait. Les fonctions
`webfront.inspector.*` ne sont pas enregistrées : leurs compteurs ne mesureraient que l'inspecteur. Les autres fonctions
réservées (`webfront.developer.*`, `webfront.commands.*`) le sont, et le panneau les masque par défaut.

**Signature.** Elle s'écrit `(a: number, b: string) => Promise<string>` : les types sont ceux que JavaScript donne ou
reçoit par le pont (`boolean`, `number`, `string`, le tableau typé d'un conteneur de nombres, `Uint8Array` pour un
`std::bitset`, `[A, B]` pour un tuple ou une paire, `T[]` pour une liste, `void`, et `unknown` pour un type que le pont
ne transporte pas) ; l'appelant `UI` n'est pas un paramètre de JavaScript et n'y figure pas ; le résultat est toujours
une `Promise`, celle de la valeur de la tâche pour une fonction différée. Les noms des paramètres, que C++ ne donne pas,
sont `a`, `b`, … La signature est bornée à 512 octets.

**Compteurs.** Chaque appel reçu incrémente `appels` ; un appel qui ne rend pas de résultat incrémente `échecs` et
remplace la dernière erreur (texte borné à 256 octets, caractères de contrôle rendus visibles) : arguments mal formés,
exception du callback, résultat qui ne s'encode pas, et, pour une fonction différée, tâche qui échoue ou qui est arrêtée
(déconnexion du navigateur, arrêt de l'application : le code `webfront.cancelled`). La durée va de la réception à la
réponse, donc jusqu'à la fin de la tâche pour une fonction différée.

**Coût.** Sans inspecteur, un appel coûte un incrément atomique relâché et la lecture d'un drapeau, sans horloge ni
verrou : une douzaine de nanosecondes mesurées (`introspection_benchmark`, Release), contre plusieurs microsecondes
pour un appel du pont. Chaque lecture de `functions` ouvre la mesure des durées pour cinq secondes ; le premier appel
qui suit cette fenêtre, et qui lit l'horloge de toute façon, la referme. Un appel mesuré coûte deux lectures d'horloge
(environ 70 ns). Une construction de distribution n'enregistre rien et ne compte rien.

**Fonctions JavaScript appelées par C++.** Une fois l'environnement développeur monté, chaque appel de `jsFunction`
(typé ou non), chaque démarrage d'un `jsFunctionSender` et chaque `broadcast` (compté une fois, quel que soit le nombre
de navigateurs) incrémente le compteur de son nom ; un appel typé dont le futur ou l'émetteur se termine en erreur
incrémente ses échecs (une annulation locale n'est pas un échec). Les noms sont comptés un à un jusqu'à 1 024 ; les
appels des noms suivants sont comptés ensemble, pour qu'une application qui fabrique ses noms ne fasse pas croître la
table sans borne. Avant le montage de l'environnement, et en distribution, aucun appel n'est compté.

**Pages.** Trois fonctions réservées, soumises au prédicat de la section 2, rendent des pages bornées comme celles de
la section 3 (au plus 1 000 lignes, réduites à `responseBudget`, chaque ligne bornée de sorte qu'une page d'une ligne
tienne dans `minPageBytes`) :

- `webfront.inspector.functions(après, nombre)` rend `(lignes, suivant)`. Une ligne est le tuple `(identifiant, nom,
  signature, fichier:ligne, espace, drapeaux, appels, échecs, dernière erreur, durée moyenne en ms, durée maximale en
  ms)` ; les drapeaux valent 1 (reçoit l'appelant), 2 (réponse différée), 4 (au moins un appel mesuré : sans ce bit les
  durées ne signifient rien). Le curseur est l'identifiant de la dernière ligne servie, stable sous les retraits ;
- `webfront.inspector.jsFunctions(après, nombre)` rend `(lignes, suivant)`, une ligne étant `(identifiant, nom, appels,
  échecs des appels typés)` ;
- `webfront.inspector.browsers(à partir de, nombre)` rend `(lignes, suivant, tâches)`. Une ligne est `(identifiant,
  drapeaux, chemin, espace, écritures en attente, résultats attendus d'appels typés de C++, appels du navigateur sans
  réponse)` ; les drapeaux valent 1 (vue prouvée), 2 (client local, `isLocalClient`), 4 (démarré), 8 (fermé, en cours de
  libération), 16 (la connexion qui demande). L'identifiant d'une connexion peut valoir 0 : le curseur est donc le
  premier identifiant de la page suivante, 0 à la fin. `tâches` est le nombre de tâches de la portée du serveur qui ne
  sont pas terminées (connexions, boucles du pont, coroutines de l'application).

Les commandes de l'application (`Commands.hpp`) se lisent par `webfront.commands.list`, qui existe déjà et garde le
contrôle d'origine (section 2). L'application lit le même registre sans le pont par `BasicWF::registeredFunctions()`.

### 8. Une page, trois conteneurs

Les inspecteurs sont une seule page de l'environnement, `inspector.html`, qui parle à C++ par le pont.

**Le conteneur se choisit sur l'accès effectif, pas seulement sur ce qui s'ouvre.** Une fenêtre qui s'ouvre mais dont
la connexion serait refusée n'est jamais choisie. Avant d'ouvrir quoi que ce soit, la page hôte appelle
`webfront.inspector.containers()`. C'est la seule fonction réservée qui ne vérifie pas l'accès : elle ne rend que trois
booléens, et aucun état de l'application.

| Booléen | Vrai quand |
| --- | --- |
| `secondaryWindow` | la page hôte peut effectivement ouvrir la fenêtre : le frontend le permet (`canShowSecondaryWindow()`) **et** la connexion hôte est admise, puisque `openWindow` vérifie l'accès comme toute fonction réservée |
| `host` | la connexion de la page hôte est admise (section 2) |
| `newConnections` | une nouvelle connexion non prouvée serait admise : l'application a choisi `trustLoopbackClients` |

Un panneau s'ouvre là où le projet le garde : dans la fenêtre flottante tant qu'il est rattaché, ce qui est son état
initial, et dans son conteneur détaché une fois détaché, tant que ce conteneur reste disponible (sinon la fenêtre
flottante sert de repli). L'ordre qui suit choisit le conteneur d'un panneau **détaché** :

1. **Fenêtre native secondaire** si `secondaryWindow`, donc seulement depuis une page hôte admise (WebKitGTK,
   WKWebView, WebView2). La fonction réservée `webfront.inspector.openWindow` appelle `openSecondaryWindow`, qui
   transmet la preuve de vue à la nouvelle fenêtre : elle ouvre sa propre connexion, prouvée. Rouvrir l'inspecteur ramène sa fenêtre au premier plan.
2. **Onglet du navigateur système** (`window.open` vers la même origine) seulement si `newConnections` est vrai et que
   la page hôte tourne dans le navigateur système. L'onglet ouvre sa propre connexion, non prouvée, que seule cette
   confiance admet. Sous CEF ou dans une vue intégrée, `window.open` réussirait mais ouvrirait un cadre sans preuve :
   ce conteneur n'y est jamais essayé. Si le bloqueur de fenêtres refuse l'ouverture, on passe au conteneur 3 et un
   message propose de réessayer.
3. **Fenêtre flottante dans la page** si `host` : le composant de la fenêtre du terminal, avec un iframe de
   `inspector.html`. C'est le conteneur de CEF, d'une vue intégrée sans fenêtre secondaire, et du navigateur système
   quand l'onglet est bloqué. Les adaptateurs n'injectent la preuve que dans le cadre principal, si bien qu'une
   connexion ouverte par l'iframe ne serait pas prouvée. L'iframe, de même origine, **n'ouvre donc pas de connexion** :
   il emprunte le pont de la page hôte (`window.parent.webFront`), dont l'accès vient d'être constaté.
4. **Aucun** sinon (navigateur système sans `trustLoopbackClients`) : les entrées du menu sont désactivées et disent
   pourquoi (section 2).

La page hôte ne sait pas d'elle-même si elle tourne dans le navigateur système : `containers()` décrit l'accès, pas la
vue. Elle le demande à `webfront.inspector.embedded()`, qui vérifie l'accès comme les autres fonctions et répond vrai
quand l'appelant est une vue prouvée (`UI::isEmbeddedView()`) **ou** quand le frontend actif transmet la preuve à ses
vues. La preuve désigne une vue prouvée, pas l'endroit où tourne une page qui ne l'a pas : sous un tel frontend, une page
admise sans preuve (un cadre d'une vue intégrée, avec `trustLoopbackClients`) peut se trouver dans une vue intégrée, et
on ne lui propose pas d'onglet. Un échec compte aussi comme une vue intégrée, ce qui n'essaie jamais d'onglet.

Chaque page hôte a un **identifiant** (1 à 64 lettres, chiffres, `-` ou `_`), gardé dans le stockage de session de son
onglet pour qu'un rechargement le garde. Ce stockage étant copié dans une page qu'elle ouvre, l'identifiant n'est repris
que par la page qui en détient le verrou (Web Locks, exclusif pour l'origine, libéré quand la page disparaît) ; sans
Web Locks, chaque chargement en tire un nouveau. Le nom de l'onglet d'un panneau détaché nomme aussi l'hôte, pour qu'une
autre page du même groupe de navigation ne réutilise pas son onglet. Un panneau détaché a sa propre fenêtre, nommée par la page `inspector.html?panel=<id>&host=<hôte>` :
`openWindow(panneau, hôte, largeur, hauteur)` n'accepte que les panneaux du menu et un identifiant valide, et
`webfront.inspector.closeWindow(panneau, hôte)` ferme celle d'un panneau rattaché de nouveau. Les deux s'exécutent sur
le thread de l'interface, où `openSecondaryWindow` et `requestClose(page)` ne retiennent pas le thread du serveur. Une
fenêtre détachée demande son rattachement par un `BroadcastChannel` de même origine, en nommant son hôte : seule la page
qui l'a détachée le prend, les autres pages de l'application ouvertes sur la même origine l'ignorent. C'est cette page
qui ferme la fenêtre, quand elle le peut : une fenêtre native par `closeWindow` sur sa propre connexion, et un nouveau
détachement n'ouvre la fenêtre qu'une fois cette fermeture terminée ; un onglet qu'elle a ouvert, par sa référence. Une
fermeture demandée par la fenêtre elle-même pourrait sinon refermer celle qu'un détachement fait entre-temps vient de
réutiliser. Un onglet dont elle n'a pas la référence (ouvert avant un rechargement) se ferme lui-même après la réponse,
et le détachement suivant ouvre un onglet sous un autre nom plutôt que de réutiliser celui-ci.

La liste des observateurs se lit page après page sous la révision de la première ; une page qui répond `changé` fait
recommencer la liste, pour qu'un observateur retiré entre deux pages ne reste pas affiché sous la nouvelle révision.

Le choix de chaque panneau et la place de la fenêtre flottante sont écrits comme des **changements** du fichier du
projet, relu au moment de l'écriture et validé seulement s'il n'a pas changé depuis (sinon l'écriture recommence) :
plusieurs pages d'un même projet ne défont pas leurs choix. `webfront.inspector.readPreferences()` lit le fichier et
son jeton sous un même verrou du store, le jeton étant calculé sur les octets rendus : un contenu qui change puis revient
entre la lecture et la sauvegarde ne valide pas des choix lus sur un autre contenu. Un fichier absent donne des choix
vides avec le jeton de son absence ; un fichier présent mais illisible fait échouer l'écriture plutôt que de l'écraser.
Un détachement ou un rattachement est écrit aussitôt, la place après une courte pause. Un seul appel admis à
`webfront.inspector.savePreferences` écrit la version conditionnelle, et C++ libère sa transaction en cas d'échec ; un
changement non écrit est repris par l'écriture suivante, après les changements plus récents du même panneau ou de la
place, qui l'emportent. Si la page ferme avant que cet appel arrive, le dernier changement peut être perdu.

La même règle désactive « Détacher » quand aucun conteneur détaché ne donnerait l'accès.

Chaque panneau a une action « Détacher / Rattacher », désactivée avec sa raison quand aucun conteneur détaché n'est
disponible. Le choix est conservé par projet dans `.webfront/developer/`, comme `terminal-window.json`.

**État du pont dans la page (#684).** Le panneau **État TypeScript ▸ Pont** montre `webFront.ready` (en attente,
résolue, ou rejetée avec sa raison), l'état de la connexion, les appels `cppFunction` en attente avec leur durée, les
Promises rejetées récemment (fonction, raison, heure) et les fonctions JavaScript que C++ appelle, avec le nombre de leurs
appels. Cet état vit dans la page de l'application.

- **Sonde.** `WebFront.js` ne garde rien pour l'inspecteur : chaque appel en attente porte seulement son nom, et `ready`
  la raison de son rejet (`readyError`). Le pont a un champ `probe`, nul par défaut. Quand il est posé, `callCppFunction`
  lui confie la paire qui règle la Promise (`probe.call(nom, resolve, reject)` rend la paire à utiliser) et
  `callJsFunction` lui annonce chaque fonction appelée par C++ (`probe.jsCall(nom)`). Sans sonde, un appel coûte un test
  de nullité : ni horloge, ni table. Le format binaire ne change pas ; les ressources du cœur restent dans la liste et le
  budget de l'ADR-016.
- **Moniteur.** La page hôte (celle qui tient l'onglet, `inspector-host.mjs`) crée un moniteur (`inspector-bridge.mjs`)
  qui pose la sonde à sa première lecture et la retire cinq secondes après la dernière : un pont que personne n'inspecte
  n'enregistre rien. Il suit le pont de l'interface (le `webFront` du cadre de l'aperçu, là où l'application appelle
  C++), ou celui de la page hôte quand l'interface n'en a pas ; un cadre rechargé donne un nouvel état. Les appels en
  attente quand la sonde est posée (à la première lecture, ou après un bail échu) sont repris avec leur nom, sans durée
  connue. Les appels de l'inspecteur lui-même
  (`webfront.inspector.*`, `wfInspect.*`) ne sont pas suivis. Un appel suivi l'est jusqu'à sa réponse, même après le
  retrait de la sonde : il faut de toute façon le retirer des appels en attente, et son rejet éventuel reste donc listé. L'état est borné : 100 appels en attente (les plus
  anciens), 50 rejets (les plus récents), 100 fonctions JavaScript (les plus appelées, sur 1 000 noms comptés un par un),
  textes de 256 octets coupés sur une frontière de caractère, caractères de contrôle rendus visibles.
- **Panneau rattaché.** L'iframe de la fenêtre flottante lit le moniteur de sa page hôte
  (`window.parent.webfrontInspectorHost.bridgeState`), sans passer par C++.
- **Panneau détaché : relais par C++.** Une autre fenêtre ne voit pas la page, et `BroadcastChannel` n'est pas garanti
  entre les vues natives. Trois fonctions réservées, sous la règle d'accès de la section 2, relaient l'état :
  `webfront.inspector.bridgeSource(hôte)` annonce la page hôte et répond si un panneau attend déjà ;
  `webfront.inspector.publishBridge(hôte, lignes)` publie l'état et répond s'il faut continuer ;
  `webfront.inspector.bridge(hôte, depuis, nombre)` rend une page `(lignes, suivant, publication, âge en ms, drapeaux)`
  dans `responseBudget` (drapeaux : 1 la page est annoncée et connectée, 2 elle a publié). Une ligne est un tuple
  `(genre, nom, détail, nombre, nombre)` : une liste de tuples des types existants (exception de l'ADR-003). `publication`
  numérote l'état ; un lecteur qui la voit changer entre deux pages recommence.
- **Rien n'est publié sans lecteur.** Une lecture ouvre un bail de 15 s (plus long que la plus lente des périodes d'un
  panneau) ; si la page ne publie pas, C++ le lui demande par `wfInspect.publish()`, une fois par seconde au plus. La page
  publie alors toutes les 250 ms, et s'arrête dès qu'une publication apprend que le bail est échu. Un panneau fermé ou en
  pause laisse donc la page se taire, et la sonde quitter le pont.
- **Bornes du relais.** Un état de plus de 256 lignes est refusé (`std::invalid_argument`) sans remplacer le précédent ;
  chaque texte est borné par `boundedText` (256 octets, 16 pour le genre). C++ garde l'état de 16 pages au plus : une page
  déconnectée est oubliée, et au-delà la page lue le moins récemment cède sa place. Une page qui a perdu sa place
  s'annonce de nouveau quand son panneau est détaché, ou quand sa publication est refusée (`unknown`) ; une nouvelle
  annonce garde le bail d'un lecteur. Un état appartient à la connexion qui s'est annoncée la dernière sous cet
  identifiant : la publication d'une autre connexion est refusée (`webfront.inspector.unavailable`). Une publication sans annonce est
  refusée (`webfront.inspector.unknown`).

Les deux panneaux affichent les mêmes lignes : le panneau rattaché passe l'état du moniteur par le même encodage en
lignes que le relais.

### 9. Ressources

Rien n'entre dans les ressources embarquées du cœur : `inspector.html`, ses modules et le menu sont produits par les
scripts de l'environnement et livrés par son archive, comme le terminal. `MinimalCoreAssets` reste inchangé ; l'ajout
du menu et des inspecteurs à l'environnement passe par une nouvelle publication de celui-ci
([ADR-021](ADR-021-publication-de-l-environnement-developpeur.md)).

### 10. MCP (facultatif)

Si la story #685 est retenue, les lectures des sections 3 et 7 deviennent des outils MCP de l'application
([ADR-025](ADR-025-mcp-http-dans-l-application.md)), sous ses grants et son authentification, pour qu'un agent lise
l'état sans capture d'écran. L'écriture de la section 6 n'est pas exposée en MCP par cette décision : l'ouvrir à un agent
demanderait un grant et une décision propres.

## Options écartées

| Option | Pourquoi |
| --- | --- |
| Accès au navigateur système par défaut, comme `webfront.developer.*` | Il n'y a pas de preuve : tout pair de la boucle locale (autre utilisateur, tunnel, proxy) lirait l'état de l'application. |
| Code à usage unique dans l'URL du navigateur système | Il faudrait une décision de sécurité propre (course avec un autre processus, historique, extensions) ; écarté de ce périmètre. |
| Laisser les écritures de l'environnement au seul contrôle d'origine | Un client non prouvé réécrirait un script que la vue prouvée charge, et lirait les inspecteurs par elle. |
| Garder les écritures de l'environnement avec tous les frontends | Avec le navigateur système, qui ne prouve rien, `wf install` ou `wf build` seraient refusés sans `trustLoopbackClients` : le parcours par défaut serait cassé pour un gain nul. |
| Garder `webfront.developer.*` sans garder les commandes | Une commande C++ peut écrire dans le projet : le scénario de la vue prouvée reste ouvert. |
| Garder par défaut tous les `cppFunction` de l'application | Toute application servie au navigateur système, ou appelée par un client légitime non prouvé, serait cassée ; WebFront ne sait pas lesquels écrivent. |
| Option d'accès propre aux inspecteurs | Elle répéterait `trustLoopbackClients` sur un autre périmètre. |
| Nouveau code de type pour les nœuds ou les arbres | Rompt les deux codecs et l'environnement publié pour un besoin que les listes de tuples couvrent. |
| Arbre complet envoyé en une fois, ou poussé par C++ | Copie les grands conteneurs, coûte même quand personne ne regarde, et dépasse le tampon de la politique. |
| Lecture sans exécuteur ni verrou fourni | Une course de données silencieuse dans l'outil censé aider à les trouver. |
| Accès qui prend un verrou et rend une référence | Le verrou est relâché au retour, avant le parcours et l'écriture ; le visiteur garde l'opération sous le verrou. |
| `changes` en une seule réponse | Après plusieurs pages dépliées, une modification globale dépasserait le tampon ou `maxMessageSize`, et l'inspecteur ne pourrait plus se resynchroniser. |
| Choisir le conteneur sur le seul succès de l'ouverture | Sous CEF, `window.open` réussit et ouvre un cadre sans preuve, dont chaque appel serait refusé. |
| Promettre une page en temps constant pour tout conteneur | Faux sans accès aléatoire ; `PagedSource` donne cette garantie quand l'application en a besoin. |
| Inspecteur dans les ressources du cœur | Contraire à l'ADR-016 ; l'inspecteur n'a de sens que dans l'environnement développeur. |
| iframe ouvrant sa propre connexion | Elle ne serait pas prouvée, puisque les adaptateurs n'injectent la preuve que dans le cadre principal. |
| Écriture désignée par le seul chemin | Après une insertion avant la cible, un indice ou un rang valide désigne le voisin : la saisie s'appliquerait au mauvais élément. |
| Identifiant d'instantané implicite pour `value` | Deux lectures entrelacées aux mêmes décalages liraient silencieusement l'une dans l'autre. |
| Destruction qui n'attend jamais, quelle que soit la source | Une opération en cours toucherait un objet ou un mutex déjà détruits ; la source propriétaire obtient ce comportement sans ce risque. |
| Destruction qui abandonne l'attente après un délai | Laisserait l'opération continuer sur des références pendantes ; le délai ne fait qu'écrire un diagnostic. |
| Terminer une passe interrompue par une resynchronisation, ou faire attendre les dépliements | Une passe peut compter des milliers de pages : l'attente bloquerait l'utilisateur, la resynchronisation relirait tout l'arbre à chaque dépliement. |
| Séparer les segments d'un chemin par un octet, sans encodage | Une clé contenant le séparateur se confondrait avec plusieurs segments. |
| Refuser `std::formatter` pour borner toute la mémoire | WebFront ne sait pas ce qu'un formateur alloue ; le refuser priverait l'inspecteur des types sans description. |
| Formater la valeur entière puis la tronquer | Un formateur ou une chaîne énormes alloueraient sans limite avant la troncature. |
| Laisser finir une opération après la destruction de sa poignée depuis la source à références | L'opération reprendrait sur des objets que l'application peut détruire juste après la poignée. |
| Modification libre de toute valeur | Changerait des invariants de l'application à son insu ; l'écriture reste un choix de l'application. |

## Conséquences

- **Bénéfice :** une mise au point de l'état C++ sans débogueur, arrêt ni `printf`, et une vue navigable des paquets ;
  aucun coût en distribution et un coût nul tant qu'aucun inspecteur n'est ouvert, hormis les compteurs atomiques des
  `cppFunction`.
- **Coût accepté :** avec le frontend par défaut (navigateur système), les inspecteurs sont refusés tant que
  l'application ne choisit pas un frontend intégré ou `trustLoopbackClients`. Le menu le dit.
- **Coût accepté :** la signature publique de `cppFunction` gagne un paramètre par défaut.
- **Coût accepté :** avec un frontend qui prouve ses vues, un client local non prouvé (script, outil externe) ne peut
  plus écrire dans le projet par `webfront.developer.*` ni lancer une commande par `webfront.commands.start` sans
  `trustLoopbackClients` ; avec le navigateur système, ces
  écritures restent ouvertes, ce qui est la limite connue de ce frontend.
- **Coût accepté :** WebFront borne ce qu'il alloue pour une valeur, pas ce qu'alloue un `std::formatter` de
  l'application, ni son temps ; un formateur coûteux s'exécute en entier sous le verrou de l'application.
- **Coût accepté :** l'unicité et la stabilité des clés d'une `PagedSource` sont un contrat de l'application, vérifié
  seulement localement (résolution par `find`, doublons d'une page).
- **Coût accepté :** détruire la poignée d'une source à références depuis cette source termine le processus, avec un
  diagnostic ; la source propriétaire est la forme qui le permet.
- **Coût accepté :** l'identité d'un nœud n'est pas stable quand un conteneur change ; l'inspecteur peut montrer un
  élément voisin à la place d'un élément retiré jusqu'à la lecture suivante.
- **Coût accepté :** une page tardive d'un conteneur sans accès aléatoire coûte un parcours linéaire, sous le verrou
  de l'application, sauf si elle fournit une `PagedSource` ; une source qui rend une valeur copie l'objet à chaque
  opération.
- **Risque :** qu'un callback de l'application qui écrit dans le projet oublie de vérifier `ui.isLocalClient()` et
  rouvre la voie que la garde ferme ; c'est un contrat documenté de l'application, que WebFront ne peut pas vérifier.
- **Risque :** qu'un accès qui rend une référence sans exécuteur crée une course ; la documentation et l'exemple Life
  montrent le visiteur sous verrou et l'exécuteur.
- **Risque :** que les valeurs affichées révèlent des données personnelles dans une capture d'écran ou un partage ;
  c'est le prix d'un outil de développement, absent des constructions de distribution.

## Validation

- Tests BDD : métadonnées des fonctions et `std::source_location` ; observateurs (pagination, réduction au tampon,
  différences par connexion, passe de `changes` sur plusieurs pages pendant que l'objet change, resynchronisation
  après une page perdue, limite de la mémoire, révision ou curseur qui ne correspondent à rien de servi
  (resynchronisation), appels concurrents traités dans l'ordre, nœud de profondeur atteinte, `value` d'une chaîne plus
  grande que `maxMessageSize` par fragments (coupure UTF-8, plafond, décalage périmé, deux lectures entrelacées dont
  la suite de la première est refusée), réponses à la taille exacte de `responseBudget` avec `maxMessageSize`
  inférieur au tampon, écriture refusée comme périmée après une insertion avant la cible, un remplacement de la cible
  ou une révision ancienne, écriture par clé d'une `PagedSource`, `fold` avec la révision courante (nœud et
  descendants retirés, nouvelle révision), avec la révision d'une passe en cours (passe interrompue puis repli) et
  avec une révision ancienne (resynchronisation : rien replié, mémoire vidée, passe en cours libérée), `children` et
  `changes` avec une révision ancienne pendant une passe (même résultat), `write` avec une révision ancienne pendant
  une passe (refus `stale` sans effet : mémoire, passe et objet inchangés, puis resynchronisation à l'appel suivant),
  deux appels qui modifient la mémoire envoyés sans attendre (le second périmé), écriture qui interrompt une passe
  puis échoue sur la précondition ou sur le mutateur (réponse résolue avec le code et la nouvelle révision, objet
  inchangé, appel suivant sans resynchronisation), refus de validation (révision, analyse, non modifiable, trop long)
  qui rejettent sans interrompre la passe, `children` qui interrompt une passe avec une mémoire qui serait pleine
  après restauration (refus avant interruption), exception d'un formateur ou de `key` dans une page après une
  interruption (page servie, nœud `opaque` ou sans clé), source qui lève avant le visiteur (rejet sans effet) et après
  lui (diagnostic, réponse servie), mutateur dont le message fait 1 Mio et dont un caractère multioctet chevauche la
  borne de 256 octets (message tronqué sur une frontière de caractère, réponse dans `responseBudget` avec le budget
  minimal, révision reçue), première ouverture d'un observateur découvert par `watches` (mémoire vide, `children` de
  la racine admis, racine puis enfants dans la page et dans la mémoire, `value` ou `write` de la racine refusés avant
  ce premier appel), reconstruction après `reset` et après une resynchronisation avec la révision rendue (racine
  relue, chemins dépliés retrouvés), racine scalaire (page réduite à la racine, écriture de la racine), `fold` de la
  racine (descendants retirés, racine gardée), `watches` (page bornée, curseur par identifiant stable sous retraits et
  ajouts pendant la liste, `nombre` à 0, `changé`), observateur retiré puis réenregistré sous le même nom (nouvel
  identifiant, appels à l'ancien refusés), réponse terminale `unknown` pour une opération en cours d'une source
  propriétaire détruite, pour une opération en file sur l'exécuteur et pour un appel ultérieur, aucune Promise en
  attente après le retrait, écriture qui interrompt une passe avec la révision de la passe (acceptée si l'état affiché
  tient, périmée sinon, y compris pour une cible d'une page non servie) et avec une révision plus ancienne (refus
  `stale` sans effet), liste `[a, b, c, d]` lue en deux pages avec une insertion en tête entre elles (liste
  recommencée, `d` lu), même cas avec une `PagedSource` à clés et une insertion qui garde le nombre d'enfants (clé du
  dernier enfant déplacée), passe qui détecte un réordonnancement des clés d'un parent déplié (« enfants modifiés »,
  positions relues), premier nœud d'une page égal au parent, clé contenant `/`, `%` et des octets non ASCII (encodage
  réversible), chemin mal formé refusé, clé dont l'encodage dépasse 128 octets (position, non modifiable), clé
  dupliquée dans une page (position, non modifiable, diagnostic unique), clé que `find` résout vers un élément dont
  `key` diffère (nœud retiré), clé dupliquée entre deux pages (précondition sur l'état affiché), suppression annoncée
  seulement dans une page perdue (absente après la resynchronisation et hors de la mémoire), `reset`, requête
  d'écriture à la taille exacte de `requestBudget` avec tous ses arguments au maximum, texte d'un octet de trop refusé
  par la page puis par C++, segment très long (nom de champ long, désigné par son rang), 64ᵉ niveau et chemin proche
  de 1 024 octets, opération entière sous le verrou du visiteur, `PagedSource`, chaque genre de la section 4,
  exécuteur, poignée détruite pendant une lecture, destruction depuis l'opération d'une source propriétaire (état
  encore vivant à la reprise, même quand la poignée en était le dernier autre propriétaire), formatage borné (chaîne
  de 100 Mio copiée jusqu'au plafond seulement, formateur qui écrit plus de 1 Gio directement dans l'itérateur de
  sortie sans allocation de WebFront au-delà de la borne, troncature détectée), passe de milliers de pages avec le
  budget minimal interrompue par `children`, `fold`, `write` et un nouveau curseur 0 (entrées non servies restaurées,
  différences retrouvées à la passe suivante, aucune resynchronisation), destruction d'une source propriétaire sous le
  verrou de la source pendant qu'une opération l'attend (sans interblocage, réponse `unknown`, état encore vivant),
  destruction d'une source à références hors du verrou pendant qu'une opération l'attend (attente puis fin),
  diagnostic d'une destruction qui attend plus que le délai, `containers` d'une page non admise avec un frontend
  capable (`secondaryWindow` faux, aucun conteneur), écriture acceptée, refusée par l'analyse, refusée par le mutateur
  et non permise) ; refus à une connexion non prouvée sans `trustLoopbackClients` ; écritures `webfront.developer.*`,
  `webfront.commands.start` et `webfront.commands.cancel` refusées à une connexion non prouvée quand le frontend
  transmet la preuve (aucune commande lancée), `webfront.commands.list` admis, acceptées depuis la vue, la fenêtre
  secondaire et le pilote `webfront-wf`, inchangées avec le navigateur système ; lectures du projet inchangées ;
  `containers` pour chaque frontend et chaque réglage de confiance ; absence des fonctions hors environnement et en
  distribution.
- Test de processus : destruction de la poignée d'une source à références depuis son visiteur, qui écrit le diagnostic
  puis termine le processus.
- Test de compilation refusée, comme `FormsReflectionRejects` : une politique de tampons dont `responseBudget` est
  plus petit que `minPageBytes`, ou dont `maxWriteTextBytes` est plus petit que 1 Kio, ne compile pas avec
  l'environnement développeur, y compris quand seul `maxMessageSize` est trop petit.
- Tests d'encodage et de décodage des nœuds et des pages, et test Jasmine dans un vrai navigateur (règles de l'ADR-003),
  dont un dépliement pendant une passe de `changes` et la lecture par fragments d'une valeur plus grande que
  `maxMessageSize`.
- `DeveloperBrowserIntegration` : menu au clavier et à la souris, inspecteur en fenêtre secondaire sous WebKitGTK,
  fenêtre flottante sous CEF par le pont de la page hôte (sans ouverture d'onglet), arbre des paquets du projet Life.
- `MinimalCoreAssets` inchangé.
- À l'intégration de l'épique, passer l'état à « Acceptée » et consigner les révisions Git réelles.

## Références

- Épique #677, issue #678.
- [ADR-003](ADR-003-protocole-binaire-et-bornes.md) (types, listes, exception sans nouveau code de type, codes
  d'erreur), [ADR-004](ADR-004-appels-asynchrones-et-erreurs.md), [ADR-006](ADR-006-taches-structurees-et-arret.md),
  [ADR-008](ADR-008-contenu-de-confiance-et-origines.md), [ADR-016](ADR-016-bootstrap-comme-socle.md),
  [ADR-020](ADR-020-services-systeme.md) (règle 3), [ADR-021](ADR-021-publication-de-l-environnement-developpeur.md),
  [ADR-023](ADR-023-espace-de-noms-des-commandes-du-terminal.md), [ADR-025](ADR-025-mcp-http-dans-l-application.md).
- [Environnement développeur](../DEVELOPER_ENVIRONMENT.md), [Formulaires et inspecteurs](../FORMS.md),
  [Exécution asynchrone](../ASYNC_EXECUTION.md), [Frontends](../FRONTENDS.md).
