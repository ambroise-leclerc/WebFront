# ADR-027 : Inspecteurs de l'état de l'application

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-25 | Proposée | Ambroise Leclerc | Décision initiale (#678, épique #677) : inspecteurs réservés à l'environnement développeur, accès réservé aux vues prouvées sauf autorisation explicite, nœuds d'arbre sur les types existants du pont, lecture et différences paginées, opérations entières sous le verrou ou sur l'exécuteur de l'application, modification à la demande de l'application, page unique `inspector.html` dans un conteneur choisi sur l'accès effectif. | À renseigner après intégration. |

## État et portée

**Proposée le 2026-11-25.** Elle précède tout code de l'épique #677 (#679 à #686), dont elle fixe les contrats. Elle
ne modifie pas le format binaire du pont ([ADR-003](ADR-003-protocole-binaire-et-bornes.md)), ni la liste des
ressources embarquées du cœur ([ADR-016](ADR-016-bootstrap-comme-socle.md)), ni le contrôle d'origine du pont
([ADR-008](ADR-008-contenu-de-confiance-et-origines.md)). Elle reprend la règle d'association des vues de
l'[ADR-020](ADR-020-services-systeme.md) (règle 3) sans l'étendre.

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

Cette règle est plus stricte que celle des callbacks `webfront.developer.*`, qui ne demandent que le contrôle
d'origine. L'écart est voulu : les observateurs montrent des données de l'application, qui peuvent être personnelles
ou secrètes, et non les fichiers du projet, que le développeur a sous les yeux. Aligner `webfront.developer.*` sur
cette règle est une autre question, hors de cette décision.

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
| drapeaux | `double` | bits : modifiable (1), valeur tronquée (2), retiré (4), profondeur atteinte (8) |

L'épique décrivait cinq champs ; le sixième porte ce que l'inspecteur doit savoir sans nouvel appel (le nœud est-il
modifiable, sa valeur est-elle complète, a-t-il disparu).

**Bornes effectives.** Chaque champ a une taille maximale en octets : le chemin 1 024, le libellé et la valeur 256,
le genre 16, plus les deux nombres et les en-têtes du codec. Un nœud encodé tient donc dans `maxNodeBytes` (2 Kio), et
une page d'un seul nœud, avec son en-tête, dans `minPageBytes` (4 Kio). Un nœud dont le chemin dépasserait 64 segments
ou 1 024 octets n'est pas refusé : il est envoyé sans enfants, avec le drapeau « profondeur atteinte », et l'inspecteur
dit pourquoi il ne se déplie pas. Aucun nœud ne peut donc dépasser `maxNodeBytes`.

**Budget d'une réponse.** Une réponse doit tenir à la fois dans le tampon d'encodage et dans la taille de message :
toutes les bornes de cette décision se calculent sur `responseBudget = min(functionCallBufferSize, maxMessageSize)`, en
comptant **tous** les octets encodés du message de retour (en-tête du message et identifiant de corrélation, tuple de
la réponse, en-têtes de liste et de chaîne, nombres, booléens). Ces en-têtes ont une taille maximale fixe, connue à la
compilation (`responseHeaderBytes`). L'enregistrement des fonctions `webfront.inspector.*` vérifie, par un
`static_assert` sur les constantes de la politique, que `responseBudget` vaut au moins `minPageBytes`. Une politique
trop petite ne compile pas avec l'environnement développeur, au lieu d'échouer en cours d'inspection. La politique par
défaut (32 Kio et 64 Mio) donne un budget de 32 Kio, soit seize pages minimales.

Une requête est bornée de même : le texte d'une écriture fait au plus `responseBudget - responseHeaderBytes` octets.
L'inspecteur le vérifie avant d'envoyer, et C++ refuse un texte plus long (`webfront.inspector.tooLarge`).

Une **page** est un tuple de premier niveau `(liste de nœuds, suivant, révision, resynchroniser)`, où `suivant` est la
position ou le curseur de l'appel suivant (0 à la fin) et `resynchroniser` un booléen. L'ADR-003 permet une liste dans
un tuple de premier niveau, pas de liste imbriquée : l'arbre circule donc à plat, un niveau déplié par appel. **Toute**
réponse qui porte des nœuds est une page : aucun appel ne rend une liste dont la longueur dépend de l'état observé. Les
appels sont :

- `webfront.inspector.children(observateur, chemin, position, nombre)` : une page des enfants d'un nœud.
- `webfront.inspector.changes(observateur, révision, curseur, nombre)` : une page des différences (ci-dessous).
- `webfront.inspector.value(observateur, chemin, instantané, décalage)` : un fragment de la valeur complète d'un
  nœud tronqué (ci-dessous) ;
- `webfront.inspector.fold(observateur, chemin)` : retire de la mémoire un nœud replié et ses descendants.

Le nombre vaut 200 par défaut et 1 000 au plus. C++ réduit chaque page pour qu'elle tienne dans `responseBudget` et
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

**Mémoire par connexion.** C++ garde, pour chaque connexion et chaque observateur, la valeur affichée des nœuds envoyés,
comme le fait `forms::inspector`, et la révision de cette mémoire. Elle compte 10 000 nœuds au plus : au-delà,
`children` échoue avec `webfront.inspector.limit` et l'inspecteur propose de replier des nœuds. Elle est libérée à la
déconnexion, et `fold` en retire un nœud et ses descendants.

**Ordre et révision.** Les appels d'une connexion sur un observateur sont traités un à un, dans l'ordre de réception,
même quand ils s'exécutent sur un exécuteur ; deux Promises en vol ne se croisent donc pas. Tout appel qui modifie la
mémoire (`children`, `fold`, `write`) lui donne une nouvelle révision, la rend dans sa réponse, et l'inspecteur
l'adopte.

**Différences paginées.** Une passe de `changes` commence au curseur 0, avec la révision R que l'inspecteur détient :

1. si R est la révision de la mémoire, C++ compare la mémoire à l'état observé, en une seule lecture (section 5), et
   range l'ensemble des différences : nœuds modifiés et nœuds retirés. La mémoire prend la nouvelle valeur, sous une
   révision R′, et la première page de l'ensemble est rendue avec R′ ;
2. l'inspecteur demande les pages suivantes avec (R′, curseur) jusqu'à un `suivant` nul. Elles sont lues dans
   l'ensemble rangé, pas dans l'état courant : la passe est cohérente même si l'objet change entre deux pages. Après la
   dernière, l'inspecteur détient R′ ;
3. une révision ou un curseur qui ne correspondent pas à la passe en cours (page perdue, passe abandonnée, mémoire
   libérée) donnent une page vide avec `resynchroniser` vrai. L'inspecteur relit alors les nœuds dépliés par
   `children`, ce qui reconstruit la mémoire.

L'ensemble rangé est borné par la mémoire (10 000 nœuds) et libéré au début de la passe suivante ou à la déconnexion.

**Entrelacement avec les autres appels.** Un appel qui modifie la mémoire pendant une passe (`children` qui déplie,
`fold` qui replie, `write`) **termine la passe** : l'ensemble rangé est libéré, et la page suivante de la passe
interrompue est une page vide avec `resynchroniser` vrai. Les pages restantes ne peuvent donc jamais décrire une vue que
l'inspecteur n'a plus. Pour éviter ces resynchronisations, l'inspecteur met en file ses dépliements, replis et
écritures jusqu'à la fin d'une passe en cours ; une passe compte au plus 50 pages de 200 nœuds. Une resynchronisation
relit les nœuds dépliés : son coût est borné par la mémoire, et elle reste correcte si la file n'est pas respectée
(onglets multiples d'une même page, client modifié).

L'inspecteur interroge à une période choisie, 250 ms par défaut, et peut se mettre en pause. C'est une lecture à
l'initiative du navigateur (pull) : C++ n'envoie rien sans demande, et un inspecteur fermé ne coûte rien.

Le chemin d'un élément de séquence est son indice. Celui d'un élément de tableau associatif est son rang dans
l'itération, et son libellé est la clé formatée. Une `PagedSource` peut fournir une clé stable par élément
(`key(position)`), qui remplace alors le rang dans le chemin. Quand le conteneur change, un chemin par indice ou par
rang peut désigner un autre élément ou plus aucun : C++ le revalide à chaque lecture et, quand il ne résout plus,
signale le nœud comme retiré. En lecture, l'identité d'un nœud au fil des modifications n'est pas garantie ; une
écriture, elle, n'est appliquée que sous la précondition de la section 6.

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
tant qu'elle vit, et la poignée reste sûre après la destruction de l'application.

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
  `key(position)`, une clé stable) fournit lui-même l'accès par page : une page coûte ce que l'application en décide.
  C'est la voie d'un très grand conteneur sans accès aléatoire, ou d'un contenu calculé à la demande.

Une page tardive d'une très grande liste chaînée ou d'une `std::map` peut donc coûter un parcours linéaire, sous le
verrou de l'application ; la documentation le dit, et `PagedSource` en est le remède.

**Destruction pendant une lecture.** La destruction de la poignée retire l'observateur de la table. Elle attend la fin
d'une lecture déjà commencée sur un autre thread, et aucune lecture ne commence ensuite. Détruite sur le thread qui
exécute justement la lecture (depuis la source ou le visiteur), elle n'attend pas : la lecture en cours se termine et
son résultat est abandonné. Un appel qui vise un observateur retiré échoue avec `webfront.inspector.unknown`.

Le nom d'un observateur est unique tant que sa poignée vit ; un second `watch` du même nom lève
`std::invalid_argument`.

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
détient et `attendu` la valeur formatée qu'il affichait pour ce nœud :

1. vérifie l'accès (section 2) et que le nœud porte le drapeau « modifiable » ;
2. analyse le texte selon le genre : booléens, entiers et flottants (dans les bornes du type, et dans celles d'un champ
   de `forms::Description` quand il en a), chaînes, énumérations décrites, `std::bitset<N>` (N chiffres binaires).
   Les autres genres ne sont pas modifiables ;
3. dans la fenêtre d'accès, vérifie la **précondition** : la révision est celle de la mémoire, et le nœud cible ainsi
   que **chacun de ses ancêtres** montrent encore, dans l'objet, exactement ce que la mémoire a enregistré pour eux
   (libellé, valeur formatée, nombre d'enfants), la valeur de la cible étant aussi égale à `attendu`. Une insertion ou
   une suppression avant la cible change le nombre d'enfants d'un ancêtre ; un remplacement change la valeur ou le
   libellé de la cible. Dans ces cas, l'écriture est refusée (`webfront.inspector.stale`) et l'inspecteur relit le nœud
   avant de la proposer à nouveau ;
4. construit la nouvelle valeur dans un temporaire, puis l'applique par le mutateur ou par affectation, toujours dans
   la même fenêtre.

La précondition compare ce que l'utilisateur a vu à ce que l'objet montre, comme un « compare-and-swap » sur l'état
affiché. Elle ne peut pas distinguer deux éléments sans clé dont tout ce qui est affiché est identique : un déplacement
qui laisse inchangés le libellé et la valeur de la cible, et le nombre d'enfants de chaque ancêtre, n'est pas détecté.
L'écriture pose alors la valeur saisie sur un élément identique en tout point visible. Un nœud dont le libellé ou la
valeur affichés sont tronqués n'est pas modifiable, puisque la comparaison porterait sur un préfixe. Une application
qui a besoin d'une identité exacte fournit une `PagedSource` avec `key(position)` : le chemin porte alors la clé, et la
précondition la vérifie.

Une précondition fausse (`webfront.inspector.stale`), une analyse refusée (`webfront.inspector.parse`), un nœud non
modifiable (`webfront.inspector.readOnly`), un texte trop long (`webfront.inspector.tooLarge`) ou un refus du mutateur
(`webfront.inspector.refused`, avec le message de son exception) rejettent la Promise et laissent l'état inchangé ;
l'inspecteur affiche la raison. Un mutateur fourni par l'application doit être tout ou rien ; c'est son contrat.

### 7. Fichier et ligne d'enregistrement

`cppFunction` et `watch` prennent un dernier paramètre `std::source_location location = std::source_location::current()`.
L'inspecteur affiche le fichier et la ligne de l'enregistrement. Les appels existants compilent sans modification, y
compris la forme explicite `cppFunction<R, Args...>(nom, f)` ; le changement est visible seulement pour du code qui
nommerait la signature exacte du membre.

Le registre de chaque `cppFunction` (#681) garde le nom, la signature écrite en TypeScript comme les déclarations
générées, le nombre d'appels et d'échecs, la dernière erreur et la durée moyenne. Les compteurs sont des incréments
atomiques relâchés ; la durée n'est mesurée que pendant qu'un inspecteur est ouvert, pour qu'une application sans
inspecteur n'en paie pas le coût.

### 8. Une page, trois conteneurs

Les inspecteurs sont une seule page de l'environnement, `inspector.html`, qui parle à C++ par le pont.

**Le conteneur se choisit sur l'accès effectif, pas seulement sur ce qui s'ouvre.** Une fenêtre qui s'ouvre mais dont
la connexion serait refusée n'est jamais choisie. Avant d'ouvrir quoi que ce soit, la page hôte appelle
`webfront.inspector.containers()`. C'est la seule fonction réservée qui ne vérifie pas l'accès : elle ne rend que trois
booléens, et aucun état de l'application.

| Booléen | Vrai quand |
| --- | --- |
| `secondaryWindow` | le frontend peut ouvrir une fenêtre secondaire (`canShowSecondaryWindow()`), qui recevra la preuve |
| `host` | la connexion de la page hôte est admise (section 2) |
| `newConnections` | une nouvelle connexion non prouvée serait admise : l'application a choisi `trustLoopbackClients` |

La page se charge dans le premier conteneur qui convient :

1. **Fenêtre native secondaire** si `secondaryWindow` (WebKitGTK, WKWebView, WebView2). La fonction réservée
   `webfront.inspector.openWindow` appelle `openSecondaryWindow`, qui transmet la preuve de vue à la nouvelle
   fenêtre : elle ouvre sa propre connexion, prouvée. Rouvrir l'inspecteur ramène sa fenêtre au premier plan.
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

La même règle désactive « Détacher » quand aucun conteneur détaché ne donnerait l'accès.

Chaque panneau a une action « Détacher / Rattacher », désactivée avec sa raison quand aucun conteneur détaché n'est
disponible. Le choix est conservé par projet dans `.webfront/developer/`, comme `terminal-window.json`.

L'état du pont dans la page (#684) vit dans la page de l'application. Détaché, le panneau le lit par un relais C++ :
la page publie son état par un appel réservé, et l'inspecteur le lit comme un observateur, sous la même règle d'accès.

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
| Modification libre de toute valeur | Changerait des invariants de l'application à son insu ; l'écriture reste un choix de l'application. |

## Conséquences

- **Bénéfice :** une mise au point de l'état C++ sans débogueur, arrêt ni `printf`, et une vue navigable des paquets ;
  aucun coût en distribution et un coût nul tant qu'aucun inspecteur n'est ouvert, hormis les compteurs atomiques des
  `cppFunction`.
- **Coût accepté :** avec le frontend par défaut (navigateur système), les inspecteurs sont refusés tant que
  l'application ne choisit pas un frontend intégré ou `trustLoopbackClients`. Le menu le dit.
- **Coût accepté :** la signature publique de `cppFunction` gagne un paramètre par défaut.
- **Coût accepté :** l'identité d'un nœud n'est pas stable quand un conteneur change ; l'inspecteur peut montrer un
  élément voisin à la place d'un élément retiré jusqu'à la lecture suivante.
- **Coût accepté :** une page tardive d'un conteneur sans accès aléatoire coûte un parcours linéaire, sous le verrou
  de l'application, sauf si elle fournit une `PagedSource` ; une source qui rend une valeur copie l'objet à chaque
  opération.
- **Risque :** qu'un accès qui rend une référence sans exécuteur crée une course ; la documentation et l'exemple Life
  montrent le visiteur sous verrou et l'exécuteur.
- **Risque :** que les valeurs affichées révèlent des données personnelles dans une capture d'écran ou un partage ;
  c'est le prix d'un outil de développement, absent des constructions de distribution.

## Validation

- Tests BDD : métadonnées des fonctions et `std::source_location` ; observateurs (pagination, réduction au tampon,
  différences par connexion, passe de `changes` sur plusieurs pages pendant que l'objet change, resynchronisation
  après une page perdue, limite de la mémoire, `children`, `fold` et `write` entrelacés avec une passe (la passe se
  termine et la page suivante demande la resynchronisation), appels concurrents traités dans l'ordre, nœud de
  profondeur atteinte, `value` d'une chaîne plus grande que `maxMessageSize` par fragments (coupure UTF-8, plafond,
  décalage périmé, deux lectures entrelacées dont la suite de la première est refusée), réponses à la taille exacte de
  `responseBudget` avec `maxMessageSize` inférieur au tampon, écriture refusée comme périmée après une insertion avant
  la cible, un remplacement de la cible ou une révision ancienne, écriture par clé d'une `PagedSource`, opération
  entière sous le verrou du visiteur, `PagedSource`, chaque genre de la section 4, exécuteur, poignée détruite pendant
  une lecture et depuis l'accès, écriture acceptée, refusée par l'analyse, refusée par le mutateur et non permise) ;
  refus à une connexion non prouvée sans `trustLoopbackClients` ; `containers` pour chaque frontend et chaque réglage
  de confiance ; absence des fonctions hors environnement et en distribution.
- Test de compilation refusée, comme `FormsReflectionRejects` : une politique de tampons dont `responseBudget` est
  plus petit que `minPageBytes` ne compile pas avec l'environnement développeur, y compris quand seul `maxMessageSize`
  est trop petit.
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
