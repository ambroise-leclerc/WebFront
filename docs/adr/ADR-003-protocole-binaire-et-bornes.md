# ADR-003 : Un protocole binaire partagé, typé et borné

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2022-02-06 | Acceptée | Ambroise Leclerc | Décision initiale : encodage des appels C++ vers JavaScript. | [4fd263e](https://github.com/ambroise-leclerc/WebFront/commit/4fd263e4e19598c9b81355ec0de80aa6dda8ecb3) |
| 1.1 | 2022-02-13 | Acceptée | Ambroise Leclerc | Messages binaires et appels JavaScript vers C++. | [05d0a72](https://github.com/ambroise-leclerc/WebFront/commit/05d0a72ac6a4ccc714eb6d6563ad2dd8cad41c87) |
| 1.2 | 2022-02-22 | Acceptée | Ambroise Leclerc | Encodage et décodage des tuples. | [d88b55d](https://github.com/ambroise-leclerc/WebFront/commit/d88b55d43ab43ca029b7e8ed62f550abc8c6a7d7) |
| 1.3 | 2026-02-28 | Acceptée | Ambroise Leclerc | Politique configurable des tailles de tampons. | [0ff1363](https://github.com/ambroise-leclerc/WebFront/commit/0ff136346d2058c0136a7f06c41c111a9f6ac56e) |
| 1.4 | 2026-07-21 | Acceptée | Ambroise Leclerc | Tableaux numériques typés et contrôles de types et de bornes. | [6f10761](https://github.com/ambroise-leclerc/WebFront/commit/6f107614b021de24a27707566223d227a013d80d) |
| 1.5 | 2026-10-14 | Acceptée | Ambroise Leclerc | Rejet explicite des chaînes dépassant 65535 octets. | [d143858](https://github.com/ambroise-leclerc/WebFront/commit/d1438582a19dc3dc0975f190c7b745150a46860e) |
| 1.6 | 2026-10-16 | Acceptée | Ambroise Leclerc | Réassemblage des messages WebSocket fragmentés. | [3636c85](https://github.com/ambroise-leclerc/WebFront/commit/3636c8519b75bdfbcae8fcc8fca0c3a5f6374c1d) |
| 1.7 | 2026-10-16 | Acceptée | Ambroise Leclerc | Limite de message vérifiée dès l’en-tête, avant allocation de la charge utile. | [68f01f5](https://github.com/ambroise-leclerc/WebFront/commit/68f01f53f304ddb56155be0d47fb6fdd293de142) |
| 1.8 | 2026-10-16 | Acceptée | Ambroise Leclerc | Plafond indépendant de 125 octets pour les trames de contrôle. | [3ebc181](https://github.com/ambroise-leclerc/WebFront/commit/3ebc181fce48240aa2cecebfe4d3f8cc4966732d) |
| 1.9 | 2026-10-20 | Acceptée | Ambroise Leclerc | `std::bitset<N>` compacté dans le format Uint8Array existant, sans nouvel opcode. | [5bc294e](https://github.com/ambroise-leclerc/WebFront/commit/5bc294eb0a9ba81ae7789b425baa6e299e848d97) |
| 1.10 | 2026-10-26 | Acceptée | Ambroise Leclerc | Commande `domBatch` bornée, interpréteur embarqué optionnel et validation transactionnelle (#380). | — |
| 1.11 | 2026-10-27 | Acceptée | Ambroise Leclerc | Validation par delta, budget cumulé des noms, récupération et contrat des événements précisés (#380). | — |
| 1.12 | 2026-10-27 | Acceptée | Ambroise Leclerc | Identifiant de lot non nul, plafond d’opérations, codec canonique et nettoyage limité aux sous-arbres concernés (#380). | — |
| 1.13 | 2026-10-27 | Acceptée | Ambroise Leclerc | Rejet de PROPERTY value sur output pour préserver les identifiants de ses enfants (#380). | — |
| 1.14 | 2026-10-27 | Proposée | Ambroise Leclerc | Notifications de document et lectures groupées par les appels existants (#381). | [PR #394](https://github.com/ambroise-leclerc/WebFront/pull/394) |

## État et chronologie

**Acceptée jusqu’à la version 1.13.** L’extension 1.14 est **proposée** dans la PR #394, en attente d’intégration.
Décision initiale : **2022-02-06** (version 1.0).
Dernière révision : **2026-10-27** (version 1.14).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

Serveur et navigateur échangent appels, arguments et résultats sans dépendance à une
armature JavaScript. Leurs deux codecs doivent partager exactement codes d’opération,
longueurs et identifiants : des vérifications lacunaires ont jadis désynchronisé les
arguments et corrompu les messages.

## Décision

Que soit préservé le protocole binaire de `weblink/Messages.hpp` et de la source `src/WebFront.js`.

Note de renommage (0.4) : la source JavaScript est désormais `js/core/WebFront.js` ; ce déplacement ne modifie pas le protocole décrit ici.
Les messages d’appel et de retour portent des identifiants de corrélation sur 16 bits.
Les types pris en charge sont les scalaires, chaînes, tuples, tableaux numériques typés et `std::bitset<N>` ;
les objets JavaScript arbitraires et objets DOM ne traversent pas automatiquement le pont.

La longueur des chaînes est bornée à 65 535 octets : C++ lève `std::length_error` à l’encodage et
JavaScript produit une `RangeError`. Un résultat de callback trop long rejette la Promise.
Les tableaux utilisent leur encodage propre et ne sont pas soumis à la limite des chaînes.

Les bitsets réutilisent les en-têtes unsigned-byte courts ou réguliers : exactement
`N / 8 + (N % 8 != 0)` octets, avec le bit C++ `i` dans le bit `i % 8` de l’octet
`i / 8` (poids faible en premier). Les bits hauts inutilisés sont nuls ; le décodeur rejette
un padding non nul, un autre type d’élément, une longueur différente ou un message tronqué.
`std::bitset<0>` est un `Uint8Array` vide. Le codec utilise l’interface publique du bitset,
sans dépendre de sa représentation, de l’endianness ou de la taille des mots hôtes.
JavaScript connaît `N` par le contrat applicatif : aucun nombre de bits n’est transmis.
Les contrats Promise/future/sender et leurs erreurs restent inchangés. L’encodage possède
ses octets compactés jusqu’à leur copie dans la trame ; la taille du bitset n’est pas bornée
par le tampon fixe des en-têtes. Le coût accepté est une allocation et un parcours bit à bit
par bitset, sans optimisation par mots à ce stade.

Le transport distingue trames WebSocket et messages : qu’il réassemble les messages
fragmentés, conserve les fragments incomplets et traite les trames réunies dans l’ordre.
La politique borne chaque message à 64 MiB par défaut ; que tout dépassement annoncé soit
refusé dès l’en-tête, fragments compris. Les trames de contrôle ont leur propre plafond de
125 octets.

## Commande DOM (`domBatch`, code 5, version 1)

Cette section remplace le transport `wfDom.apply` des prototypes de l'[ADR-018](ADR-018-acces-au-dom.md) ;
l’API de document et de nœuds de #381 utilise désormais ce transport, comme décrit ci-dessous.

Un message WebSocket C++ → navigateur contient exactement un lot : `05 01`, un identifiant de lot
`uint32` **little-endian fixe non nul**, puis zéro ou plusieurs opérations jusqu'à la fin du message.
L'identifiant zéro est réservé aux rejets non corrélables, notamment un en-tête tronqué ; il n'est
jamais un identifiant de lot valide. C++ exige `DomBatch(id)` sans valeur par défaut et rejette zéro,
comme le décodeur JavaScript. Le plafond
est de **1 048 576 octets, en-tête de six octets inclus**, vérifié avant décodage/allocation par les
codecs C++ et JavaScript. Il est indépendant du plafond WebSocket (qui peut être plus restrictif).
Chaque lot contient au plus **131 072 opérations**, NAME comprises, vérifiées à l'encodage C++ et
avant application au décodage C++/JavaScript. Ce second plafond limite l'amplification mémoire des
nombreuses opérations courtes. Le décodeur C++ compte et valide la structure sans copier les chaînes,
réserve le nombre d'opérations comptées, puis décode les valeurs possédées : au plus 131 072 objets
`DomOperation` (soit `131072 * sizeof(DomOperation)` octets de valeurs), plus les chaînes et les
surcoûts des conteneurs/allocateurs, plutôt qu'une croissance géométrique jusqu'à environ 524 000 objets.
Le coût accepté est un second parcours des en-têtes. Le budget des valeurs de chaînes reste inclus
dans le plafond de message. Le retour possédé n'est pas une vue sans allocation.
Aucune compatibilité avec le prototype de l'ADR-018 n'est requise.

Les entiers des opérations sont des `uint32` LEB128 non signés **canoniques** : cinq octets au plus,
quatre bits utiles au maximum dans le cinquième, aucun groupe haut nul superflu. Une chaîne est sa
longueur en octets LEB128, puis ses octets UTF-8 stricts (pas de surrogates, surlongueurs ni code point
au-delà de U+10FFFF ; un BOM initial est conservé). L'opcode et les flags sont des octets bruts.

| Code | Opération | Opérandes dans l'ordre |
| --- | --- | --- |
| 1 | NAME | index, chaîne |
| 2 | ELEMENT | identifiant, index du tag |
| 3 | TEXT | identifiant, chaîne |
| 4 | ATTRIBUTE | identifiant, index du nom, chaîne |
| 5 | UNSET_ATTRIBUTE | identifiant, index du nom |
| 6 | PROPERTY | identifiant, index du nom, chaîne |
| 7 | SET_TEXT | identifiant de **nœud texte**, chaîne |
| 8 | APPEND | parent, enfant |
| 9 | INSERT_BEFORE | parent, enfant, référence |
| 10 | REMOVE | identifiant (oublie tout le sous-arbre) |
| 11 | LISTEN | identifiant, index de l'événement, flags |
| 12 | UNLISTEN | identifiant, index de l'événement |
| 13 | MOUNT | identifiant, index du nom de l'îlot |

Les flags de LISTEN sont `1 = preventDefault`, `2 = stopPropagation` ; tous les autres bits sont
interdits. PROPERTY accepte `value` (chaîne), `checked`, `disabled`, `indeterminate` (exactement
`true` ou `false`), et `selectedIndex` (entier décimal de -1 à 2³¹-1). Les objets ne traversent pas le pont.
`PROPERTY value` est rejeté sur `output`, même vide : son setter natif remplace les enfants et
invaliderait les identifiants suivis. Créer un nœud texte enfant et le mettre à jour par SET_TEXT.
Les identifiants de nœuds sont non nuls et uniques parmi les nœuds vivants. L'interpréteur borne les
nœuds vivants et les noms à 65 536 entrées chacun, les noms à 1 024 octets UTF-8, et la somme des
longueurs UTF-8 des noms à **1 048 576 octets par interpréteur** (hors surcoûts des chaînes et tables JS).
Une définition qui dépasse un de ces plafonds n'est pas acquise. Le budget cumulé compte aussi les
noms acquis dans des lots rejetés. Les noms sont conservés pour la vie du document, sans purge ni
réinitialisation sur le fil : les réutiliser permet de continuer à muter après saturation, mais ajouter
de nouveaux noms exige un nouveau document/rechargement. L'émetteur doit tenir le miroir des indices,
des longueurs et des définitions acquises ; le codec `msg::DomBatch` ne constitue pas une table d'internement.
Le helper C++ de document reste dans #381. Un rechargement impose de reconstruire également ce miroir.
Après un rejet sémantique, le lot corrigé réemploie les indices déjà acquis : il ne renvoie pas les
mêmes NAME. Par exemple, si NAME(3, `data-ready`) précède une opération rejetée, la reprise utilise
ATTRIBUTE(..., 3, ...) ; renvoyer NAME(3, ...) est une redéfinition hors séquence et sera rejeté.

Les noms sont internés par document navigateur, définis séquentiellement à partir de zéro. Une
référence ne peut viser qu'une définition antérieure dans le flux. Toutes les définitions entièrement
décodées restent acquises même si la suite du lot est rejetée, y compris lors d'une erreur sémantique.
Un message mal formé ne garantit pas que ses définitions restantes soient acquises ; l'émetteur doit
alors abandonner ce document. Les autres opérations sont validées contre un arbre temporaire qui
suit créations, déplacements et suppressions. Cet arbre est un delta sur la table vivante : seuls les
identifiants et ensembles d'enfants modifiés sont enregistrés ; un parent large utilise lui aussi des
ajouts/suppressions différés. MOUNT consulte un index faible des éléments montés. Un petit lot de texte
ou d'attributs ne parcourt donc pas les nœuds non concernés. Les suppressions parcourent le sous-arbre
affecté et les déplacements vérifient les chaînes d'ancêtres. Les écouteurs sont indexés par identifiant
de nœud : REMOVE ne désabonne que les identifiants supprimés. La révocation d'un îlot part de son
identifiant monté et parcourt ses enfants ; elle ne scanne ni les arbres détachés ni leurs ancêtres.
Son coût est linéaire dans son sous-arbre et ses abonnements. Ces limites bornent les ressources, sans
promesse de temps constant. Une erreur de validation ne modifie aucun nœud DOM.
Un parent texte, un cycle, une référence INSERT_BEFORE qui n'est pas enfant du parent, la réutilisation
d'un nœud supprimé et toute mutation/déplacement/suppression de l'élément d'îlot sont rejetés.

Le module embarqué distinct `/webfront-dom.mjs` exporte `installDomInterpreter(webFront, extensions)`.
L'application le charge explicitement et prête des éléments HTML autorisés, vides et disjoints par
`interpreter.island(nom, élément)` ; la fonction retournée révoque les identifiants et écouteurs du
sous-arbre. Réenregistrer le même nom et élément conserve les nœuds ; une ancienne révocation ne
révoque pas une inscription ultérieure. Seuls les enfants sont détenus par C++ ; MOUNT ne déplace
pas l'élément prêté. Les nœuds détachés créés par un lot restent connus jusqu'à REMOVE ou à la fin du document.
L'application conserve la responsabilité de révoquer un îlot avant que la page en modifie le sous-arbre.

La construction utilise exclusivement `createElement` et `createTextNode`, jamais HTML. Les listes
blanches par défaut du module couvrent les éléments HTML usuels, attributs, propriétés et événements.
L'application peut ajouter des tags, attributs et événements par les tableaux `extensions.tags`,
`extensions.attributes`, `extensions.events` (copiés à l'installation). Les noms sont minuscules,
sans namespace ; `script`, les conteneurs exécutables (`iframe`, `object`, `embed`, etc.), les attributs
`on*`, `srcdoc`, `is`, `xmlns`, et les attributs multi-URL `srcset`/`ping` restent interdits.
Les attributs URL simples (`href`, `src`, `action`, etc.) passent par `new URL(valeur, document.baseURI)` ;
seuls `http:`, `https:` et `mailto:` sont autorisés, même dans une extension. `javascript:` et `data:`
sont donc rejetés après normalisation des caractères de contrôle par le navigateur.

Le modèle de menace est celui de l'[ADR-008](ADR-008-contenu-de-confiance-et-origines.md) : l'application
C++ et le code de la page sont de confiance. Les listes blanches empêchent des constructions exécutables
accidentelles ; elles ne constituent pas un bac à sable pour des lots provenant d'un tiers. `style`
peut charger des URL CSS externes et positionner des surcouches ; `id` et `name`, notamment sur `img` et
`form`, peuvent masquer des propriétés nommées du document ou de la fenêtre (DOM clobbering). Ils sont
conservés pour construire des interfaces ordinaires. Les restrictions des URL décrites ci-dessus visent
les attributs URL, pas le contenu CSS. Ne pas transmettre des lots non fiables à cet interpréteur ; les
politiques d'origine/navigations ne filtrent pas toutes les sous-ressources réseau.

Les erreurs de format, d'UTF-8, de flags, d'identifiant zéro et d'opérandes C++ inutilisés non nuls
produisent `std::invalid_argument`. Les dépassements de taille de message ou de nombre d'opérations
produisent `std::length_error`. À l'encodage C++, les champs `b`/`c` absents du format de l'opération
doivent être zéro et une valeur de chaîne absente doit être vide ; ils ne sont jamais ignorés.
Une erreur d'ajout n'ajoute aucun préfixe ni opération au lot.

C++ construit `msg::DomBatch` (`weblink/DomBatch.hpp`) et l'envoie par `ui.sendDomBatch(batch)` sur le
contexte du bridge. L'envoi n'est pas attendable et n'accuse pas réception. Tout rejet, y compris
l'absence de module installé ou une exception DOM résiduelle pendant l'application, remonte par le
callback ordinaire `wfDom.rejected(uint32 batchId, string reason)` ; l'application doit l'enregistrer.
Le motif est limité à 4 096 unités UTF-16 avant encodage. Si l'en-tête est tronqué, l'identifiant vaut zéro.
Les événements utilisent `wfDom.event(uint32 nodeId, uint32 nameIndex, string value)` et leurs
Promises sont traitées. Une valeur d’événement dépassant 65 535 octets UTF-8
n’est pas livrée à C++ et n’est pas tronquée ; le rejet d’encodage, de transport ou
de callback est journalisé dans la console du navigateur. Il n’existe pas de
notification C++ distincte pour cet échec. `nameIndex` désigne le nom interné à LISTEN. La valeur est celle du nœud
abonné (`event.currentTarget`), pas d'un descendant à l'origine d'un événement bouillonnant :

| Événement | `value` |
| --- | --- |
| `input`, `change` sur une case à cocher | `String(currentTarget.checked)`, soit `"true"` ou `"false"` |
| Autres `input`, `change` | `String(currentTarget.value ?? "")` (vide si la propriété est absente) |
| `keydown`, `keyup` | `event.key` |
| Tout autre événement | La chaîne vide `""` |

Les flags `preventDefault` et `stopPropagation` sont appliqués immédiatement dans le navigateur,
avant le callback C++ ; UNLISTEN et REMOVE retirent les écouteurs. Les erreurs de transport/déconnexion ne peuvent pas être signalées par ce canal.
Une exception résiduelle d'application peut laisser des modifications partielles ; elle invalide tous
les handles et îlots de l'interpréteur. Les fonctions `release()` déjà retournées deviennent inertes :
elles n'effacent pas les enfants partiels et ne notifient pas la page. Le rejet arrive seulement au
callback C++ ; l'application doit en organiser la notification de sa page. Pour récupérer, la page
remplace les éléments d'îlot, ou vide explicitement leurs enfants (`element.replaceChildren()`), puis
les prête de nouveau avec `island()`. Prêter un ancien élément encore non vide reste interdit.
Les identifiants de nœuds sont à reconstruire, mais la table de noms et son budget restent acquis.
La page peut aussi choisir de recharger tout le document pour remettre les deux tables à zéro.
L’API publique document/nœuds utilise ce flux et les appels ordinaires existants (#381).
`dom.connect()` installe `wfDom.read` dans le navigateur puis attend `webFront.ready`
avant d’appeler `wfDom.ready()` côté C++. Cet appel est idempotent sur une connexion
vivante : réessayer un acquittement perdu ne remet ni noms ni nœuds à zéro. Le mode connecté n’est acquis qu’après
l’acquittement ; un échec de cette négociation permet de réessayer. Les révocations
survenues pendant l’attente sont envoyées après acquittement. La connexion n’est
complète qu’après acquittement de toutes les révocations ; une reprise conserve la
négociation et les révocations déjà acquittées, sans répéter `wfDom.ready`. Les
révocations non acquittées et celles survenues entre tentatives restent en attente. `release()` envoie `wfDom.revoked(string islandName)`
quand ce mode est actif. Le document C++ reçoit les rejets et les événements de sa connexion ;
les utilisateurs bas niveau sans `connect()` gardent leurs propres callbacks.

Une lecture groupée appelle `wfDom.read(Uint32Array pairs)` : chaque paire contient
l’identifiant de nœud puis la propriété (0 textContent, 1 value, 2 checked, 3 largeur,
4 hauteur du rectangle en pixels CSS). Le groupe contient 1 à 4 096 paires, toutes
validées avant lecture. Le résultat est un Uint8Array d’au plus 1 MiB : pour chaque
requête, longueur UTF-8 uint32 little-endian puis les octets de sa valeur convertie
en chaîne (propriété absente : chaîne vide). Les frontières et le nombre exact de
résultats sont contrôlés en C++. Les erreurs rejettent l’appel ordinaire. Aucun nouveau
code de type du bridge n’est ajouté. L’arrêt sender reste local et ignore une réponse tardive.
Voir [DOM.md](../DOM.md) pour les règles d’identité et d’exécution.

## Options écartées et motifs

- JSON/RPC ou sérialisation automatique des objets : non retenus dans le protocole intégré ;
  ils changeraient son contrat de types et nécessiteraient une décision explicite.
- Tronquer silencieusement une longueur : comportement défectueux corrigé, à ne pas restaurer.
- Assimiler une réception réseau à une trame ou une trame à un message : incompatible avec
  les découpages et regroupements permis par WebSocket.

## Conséquences

- **Bénéfice :** contrat compact et tableaux numériques utilisables sans conversion en texte.
- **Coût accepté :** maintenance de deux codecs et limites visibles aux utilisateurs.
- **Coût proposé en 1.14 :** état optimiste par connexion, tâche d’envoi cadencée et mémoire
  des nœuds côté C++. Un rejet ou une révocation avec des écritures en attente impose
  un rechargement ; l’arrêt sender ne peut pas interrompre une lecture déjà envoyée au navigateur.
- **Risque :** dépassements, mauvais types ou allocation excessive ; contrôler les bornes avant
  lecture/allocation et exercer les décodeurs avec sanitizers et fuzzing.

## Prescriptions d’implémentation et de génération de code

- Que les codecs C++ et JavaScript évoluent de concert ; que `IndexFS.hpp` soit régénéré
  depuis les sources. Exception : un changement purement C++ qui réutilise un encodage
  existant, sans nouveau code de type ni modification du format binaire, peut laisser
  le codec JavaScript et `IndexFS.hpp` inchangés. Les tests doivent alors démontrer la
  compatibilité avec le codec JavaScript existant dans un navigateur réel.
- Que toute évolution du protocole s’accompagne de tests d’encodage et de décodage ainsi que
  d’une régression en navigateur réel, erreurs et frontières de taille comprises. Pour 1.14,
  `DomDocumentTests.cpp` et les suites Jasmine `dom-document-specs.mjs`/`dom-specs.mjs`
  couvrent identités, durées de vie, arrêt local, callbacks bas niveau avant connexion, lectures et bornes.
- Ne pas contourner une limite de chaîne par un cast de longueur ; découper les données ou
  employer un tableau numérique lorsque le contrat fonctionnel le permet.
- Garder `BuffersPolicyType` et ses valeurs de repli compatibles avec les politiques existantes.
- Rejeter les continuations mal placées et les trames de contrôle fragmentées ou trop longues.

## Sources et vérification

- [Codec C++](../../include/webfront/weblink/Messages.hpp), [codec JS](../../js/core/WebFront.js),
  [politique de buffers](../../include/webfront/http/BuffersPolicy.hpp).
- Tests : [MessagesTests](../../test/unit/MessagesTests.cpp), [WebSocketTests](../../test/unit/WebSocketTests.cpp),
  [spécifications navigateur](../../test/web/suite/specs/webfront-specs.mjs).
- Voir [ADR-004](ADR-004-appels-asynchrones-et-erreurs.md) et [ADR-012](ADR-012-preuves-et-tests.md).
