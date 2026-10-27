# ADR-018 : Accéder au DOM depuis C++ par un flux de mutations borné, sans disputer le DOM aux bibliothèques d'interface

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-26 | Proposée | Ambroise Leclerc | Décision initiale (#378) : état de l'art TypeScript, Rust et Python vérifié, huit solutions comparées, prototypes mesurés sous WebKitGTK (flux de mutations, proxy, lectures attendues, îlots React, formulaire généré par réflexion C++26, lots hostiles), découverte d'un délai de 40 ms du transport (Nagle). | À renseigner après intégration. |
| 1.1 | 2026-10-27 | Proposée, partiellement remplacée | Ambroise Leclerc | Le transport du prototype est remplacé par ADR-003 1.10 et suivantes ; l’API document/nœuds reste proposée (#381). | — |
| 1.2 | 2026-10-27 | Proposée, partiellement livrée | Ambroise Leclerc | API document/nœuds, cycle de 1 ms, événements et lectures groupées (#381). | — |

## État et chronologie

**Proposée** le **2026-10-26** (issue #378), **partiellement remplacée** le **2026-10-27**.
Le transport DOM est désormais livré par l'[ADR-003, version 1.10 et suivantes](ADR-003-protocole-binaire-et-bornes.md),
avec la commande `domBatch` et `/webfront-dom.mjs` (#380). L'appel `wfDom.apply` par `jsFunction` décrit
ci-dessous est uniquement le protocole historique des prototypes ; il ne décrit pas le transport livré.
L’API document/nœuds est livrée par #381 ([DOM.md](../DOM.md)) ; les adaptateurs et la réflexion
restent proposés (#382 à #384). Le point d’envoi retenu est un cycle serveur de 1 ms
après la première écriture en attente, au sein d’une tâche possédée et drainée.
`flush()` et les lectures explicites peuvent envoyer plus tôt. `SET_TEXT` reste limité
aux nœuds texte. Une révocation invalide le sous-arbre ; avec des écritures en attente,
le document ferme plutôt que d’envoyer un lot contenant des identifiants révoqués.
Une déconnexion ou l’arrêt invalide l’ensemble du document. Toutes les opérations
s’exécutent sur le contexte serveur ; les lectures sont des senders groupés annulables
localement, sans nouvelle catégorie de valeur sur le bridge.
Les mesures historiques viennent de prototypes jetables, retirés du dépôt après mesure et consultables dans le commit
`ef2bb25` de la PR #385 (`test/web/dom/`, cible manuelle `webkitgtk_dom_prototype`). Elle passera à « Acceptée » quand les
critères de la section dédiée seront démontrés.

WebFront n'est pas encore en alpha : la décision peut faire évoluer le format de l'[ADR-003](ADR-003-protocole-binaire-et-bornes.md)
sans compatibilité ascendante, à condition d'en respecter les règles (évolution conjointe de `WebFront.js` et
`Messages.hpp`, tests d'encodage et de décodage, test dans un vrai navigateur, mise à jour de l'ADR-003).

## Contexte

Lors de la décision initiale, le C++ n'accédait pas au DOM. Une application écrit du JavaScript ou du TypeScript (par exemple
`examples/life/ui/main.tsx`, en React), qui expose des fonctions que C++ appelle par `ui.jsFunction`, et appelle les
callbacks C++ par `webFront.cppFunction`. Le bridge ne transporte que des scalaires, chaînes, tuples, tableaux typés
numériques et `std::bitset<N>` ([ADR-004](ADR-004-appels-asynchrones-et-erreurs.md)). Les anciennes issues #6
(`class WebFront : public DOM::Body`) et #28 (un objet `UI` représentant le document) sont fermées ; #266 (réflexion
statique pour les composants) a été fermée non planifiée, avec la consigne de partir d'un cas d'usage concret, de
spécifier séparément compatibilité et exigences compilateur, et de couvrir le protocole.

Ce que l'accès au DOM permettrait, et que la voie actuelle rend répétitif :

- **Des IHM par construction** : panneaux de réglage, formulaires, inspecteurs d'état et tableaux de bord générés à
  partir de types C++, avec libellés, bornes et validation issus du type.
- **De petites applications entièrement en C++** (instruments, outils internes), sans projet TypeScript.
- **Des emplacements C++ dans une interface existante** : l'application pilote un panneau à l'intérieur d'une page
  React (ou Solid, Svelte, Vue, Lit) sans écrire la glue JavaScript de chaque élément.
- **Des tests d'interface depuis C++** : construire, déclencher et vérifier sans spec JavaScript dédiée.

Contraintes à respecter : les appels sont asynchrones et asymétriques ([ADR-004](ADR-004-appels-asynchrones-et-erreurs.md)) ;
les tâches sont possédées et jointes ([ADR-006](ADR-006-taches-structurees-et-arret.md)) ; le bridge ne promet pas de
bac à sable ([ADR-008](ADR-008-contenu-de-confiance-et-origines.md)) ; le chemin HTML reste normatif, y compris pour
de futurs micro-contrôleurs ([ADR-017](ADR-017-acces-direct-aux-frontends.md)) ; les ressources du cœur forment une
liste fermée sous 150 KiB ([ADR-016](ADR-016-bootstrap-comme-socle.md)).

## État de l'art (sources vérifiées le 2026-10-26)

### TypeScript et JavaScript

- **React** possède son DOM et le réconcilie. Un moteur de rendu personnalisé passe par `react-reconciler`, dont le
  README avertit : « Its API is not as stable as that of React, React Native, or React DOM, and does not follow the
  common versioning scheme ». Deux modes existent : *mutation* (`supportsMutation`, cibles de type DOM avec
  `createInstance`, `appendChild`, `commitUpdate`) et *persistent* (arbres immuables, Fabric).
  `useSyncExternalStore(subscribe, getSnapshot)` est la voie documentée pour lire un état extérieur : l'instantané doit
  rester identique (`Object.is`) tant que l'état ne change pas.
- **Réactivité à grain fin** (Solid, Svelte 5, Vue, Angular signals) : le composant s'exécute une fois et des effets
  mettent à jour des nœuds précis. La proposition **TC39 Signals** est au *Stage 1* (« can currently be thought of as
  Stage 0 ») : `Signal.State`, `Signal.Computed`, `Signal.subtle.Watcher`, **sans** `effect()` ni ordonnancement, laissés
  aux bibliothèques.
- **Shopify Remote DOM** construit un arbre dans un environnement isolé (iframe, worker) et le réplique dans la page
  hôte : `RemoteMutationObserver` côté distant, `DOMRemoteReceiver` côté hôte, avec une **liste blanche** par élément
  (`attributes`, `events`, `methods`) ; « omitting it leaves element names unrestricted ». Les fonctions ne passent pas
  par le flux de mutations : une bibliothèque RPC séparée est nécessaire.
- **Phoenix LiveView** garde l'état par connexion sur le serveur, envoie des diffs et les applique avec `morphdom`
  (`import morphdom from "morphdom"` dans `dom_patch.ts`) ; `phx-update="ignore"` laisse un sous-arbre à une
  bibliothèque JavaScript. **htmx** échange des fragments HTML sans état.

### Rust

- **Leptos** : « The body of the component function is a set-up function that runs once, not a render function that
  reruns multiple times » ; un signal modifié produit « a targeted update to that specific text node ».
- **Dioxus** : arbre virtuel dont le cœur émet un flux de `Mutation` (15 variantes : `LoadTemplate`, `CreateTextNode`,
  `SetAttribute`, `SetText`, `NewEventListener`, `AppendChildren`, `InsertBefore`, `Remove`, `PushRoot`, …) adressant les
  nœuds par `ElementId` numérique, avec des gabarits chargés une fois et des chemins de 255 enfants au plus. Un même
  interpréteur JavaScript applique ce flux pour le web, le bureau et LiveView. C'est le modèle le plus proche de WebFront.
- **Yew** reconstruit un arbre virtuel et le compare ; **wasm-bindgen/web-sys** génère des liaisons typées depuis WebIDL.

### Python

- **PyScript / Pyodide** : Python dans le navigateur ; le FFI « creates proxy objects in Python that are linked to
  actual objects in JavaScript » (`pyscript.window`, `pyscript.document`), et `pyscript.web` en est l'enveloppe idiomatique.
- **Reflex** compile l'interface en application React ; chaque événement est « sent to the backend through a WebSocket
  connection », l'état vit sur le serveur par jeton client et seules les variables modifiées (« dirty vars ») reviennent.
- **NiceGUI** maintient les éléments côté serveur (composants Vue/Quasar) ; sa classe `Outbox` (`nicegui/outbox.py`)
  **regroupe les mises à jour par identifiant d'élément** et les émet par une boucle cadencée (`asyncio.sleep(0.1)`).

### Rendu hors DOM

Flutter web et Slint dessinent sur un canvas et reconstruisent l'accessibilité à part (sources secondaires, voir #378).
Ils servent de contre-exemple : WebFront garde le DOM réel et donc l'accessibilité native.

### Leçons retenues

1. Tous les systèmes d'interface distante qui tiennent la charge **regroupent** les mutations (Dioxus, Remote DOM,
   LiveView) ou les **fusionnent** par élément (NiceGUI, Reflex).
2. Les nœuds sont des **identifiants numériques** ; les noms et gabarits sont transmis une fois.
3. La sécurité est une **liste blanche côté hôte** (Remote DOM), pas une confiance dans l'émetteur.
4. Les bibliothèques d'interface gardent la propriété de leur DOM ; l'intégration passe par des **données** (store
   externe) ou des **îlots** qu'elles ne réconcilient pas (`phx-update="ignore"`).
5. Aucun système distant n'offre de **lecture synchrone** du DOM.

## Prototypes et mesures (WebKitGTK 2.52.6, Linux, 2026-10-26)

Les prototypes ne font pas partie du dépôt : ils sont consultables dans le commit `ef2bb25` de la PR #385, sous
`test/web/dom/`, hors de la bibliothèque :

- `DomPrototype.hpp` : `Document`, `Node` et `Batch`, l'encodeur C++ d'un flux de mutations (octet de version, puis
  opérations ; entiers en varint LEB128, chaînes UTF-8 préfixées, noms internés une fois par document). Le lot est
  envoyé comme un seul `Uint8Array` par le bridge existant (`ui.jsFunction("wfDom.apply")(bytes)`), donc **sans
  modification du format**.
- `dom-interpreter.js` : décode et **valide tout le lot avant d'appliquer quoi que ce soit** ; crée les nœuds par
  `createElement`/`createTextNode` uniquement ; listes blanches d'éléments, d'attributs (`aria-*`, `data-*`), de
  propriétés et d'événements ; URL limitées aux chemins relatifs, `http(s)` et `mailto`. Les événements reviennent par
  `webFront.cppFunction("wfDom.event")`. La page prête des **îlots** (`wfDom.island(nom, élément)`) dont C++ ne possède
  que les enfants.
- `react-bridge.js` : React 19.3.1 (builds de production du registre de test), un store externe alimenté par C++ et lu
  par `useSyncExternalStore`, et un composant `Island` rendu sans enfants.
- `ReflectedForm.hpp` : formulaire, déclaration TypeScript et JSON générés depuis un agrégat annoté, par réflexion C++26.
- `DomPrototype.cpp` et `dom-prototype.html` : les scénarios, les vérifications du DOM et le rapport. Le programme
  échoue sur un DOM faux ou un rapport absent, jamais sur une durée.

Exécution dans ce commit (build `-DWEBFRONT_EMBED_WEBKITGTK=ON -DENABLE_TESTING=ON`, Release) : construire la cible
`webkitgtk_dom_prototype`, extraire les archives `react-19.3.1`, `react-dom-19.3.1` et `scheduler-0.28.0` de
`build/registry-archives/` dans un répertoire de paquets, puis lancer le programme sous Xvfb depuis `test/web/dom`
avec ce répertoire en argument.

Configuration : AMD Ryzen 9 3900X, Linux 7.0, GCC 16.1.0 (`-std=c++26 -freflection`), WebKitGTK 2.52.6 sous Xvfb.
WebKit arrondit `performance.now()` à la milliseconde : les allers-retours inférieurs à la milliseconde sont des
moyennes de 100 essais, les autres valeurs des médianes de 5 passes. Les durées de la page comprennent une mise en page
forcée (`offsetHeight`). Chaque ligne compte cinq nœuds (`li`, `span`, texte, `button`, texte).

### Un délai de 40 ms dans le transport

La première série a révélé un plancher de **40 ms** sur tout appel JavaScript → C++ dont le callback envoie lui-même
un message avant de rendre son résultat (le lot, puis le `functionReturn`). Lors de ces mesures, le serveur n'activait pas `TCP_NODELAY` :
la seconde petite trame attend, sous l'algorithme de Nagle, l'acquittement de la première, que la pile du navigateur
retarde (« delayed ACK » de Linux). Une modification locale d'une ligne (option `stdnet::ip::tcp_no_delay` sur chaque
socket acceptée, non livrée lors des mesures) fait disparaître ce plancher. Le défaut touchait les applications dès qu'un
callback appelait `ui.jsFunction` avant de rendre ; #379 active désormais cette option avec des régressions sur socket réelle
et navigateur (voir [ADR-005, version 3.2](ADR-005-evolution-reseau-asio-stdnet.md)). Les deux séries historiques sont données.

| Mesure | Cœur lors des mesures | Avec `TCP_NODELAY` |
| --- | ---: | ---: |
| Construire 1 000 nœuds depuis C++ (14 513 octets, 2 404 opérations, encodage 0,05 ms) | 42 ms | 23 ms |
| Même liste construite en JavaScript local | 18 ms | 19 ms |
| Construire 10 000 nœuds depuis C++ (180 918 octets, 24 004 opérations, encodage 0,44 ms) | 221 ms | 219 ms |
| Même liste construite en JavaScript local | 197 ms | 202 ms |
| Changer 2 000 textes en un lot (32 891 octets) | 173 ms | 168 ms |
| Changer 2 000 textes, un message par opération (« proxy ») | 449 ms | 462 ms |
| 200 lectures attendues une à une (`co_await` par lecture) | 70,5 ms | 31,9 ms |
| 2 000 lectures attendues une à une | 290 ms | 311 ms |
| 200 lectures en un appel | 41,1 ms | 0,29 ms |
| 2 000 lectures en un appel | 42,1 ms | 1,24 ms |
| Clic → callback C++ → mutation appliquée (moyenne de 100 ; maximum) | 20,7 ms ; 42 ms | 0,35 ms ; 2 ms |
| Clic dans un îlot → C++ → store → rendu React (moyenne de 100) | 20,9 ms | 0,77 ms |
| 1 000 mises à jour du store depuis C++ jusqu'au rendu de la dernière | 73 ms, 1 000 rendus | 38 ms, 1 000 rendus |
| Chargement et évaluation de React 19.3.1 (660 Ko de builds) | 19 ms | 19 ms |

Autres résultats, identiques dans les deux séries :

- **Lots hostiles** : six lots (élément `script`, attribut `onclick`, `href="␠JavaScript:…"`, `src="data:…"`, propriété
  `innerHTML`, événement `mouseover`) sont tous rejetés **entièrement** : le paragraphe créé en tête de chaque lot
  n'apparaît jamais. Comme `apply` est sans retour, le rejet est signalé à C++ par `wfDom.rejected`, et C++ en a compté six.
- **Îlots React** : le sous-arbre C++ survit au double montage de `StrictMode` et à un nouveau rendu du parent ; React ne
  réconcilie pas les enfants d'un élément qu'il rend sans enfants.
- **Formulaire par réflexion** : cinq champs (entier borné, réel borné, booléen, énumération, chaîne), 92 opérations et
  789 octets ; la saisie met à jour le `struct` C++, une valeur hors `Range` est ignorée, `submit` est annulé par un
  `preventDefault` déclaré à l'abonnement.
- **Taille de l'interpréteur** : 14 676 octets commentés, 4 088 octets compressés en gzip.

### Ce que montrent les mesures

1. **Le flux regroupé coûte peu.** Construire depuis C++ coûte de 8 à 21 % de plus que le même code JavaScript local ;
   l'encodage C++ est négligeable (0,44 ms pour 10 000 nœuds) et le reste est le travail du moteur (DOM et mise en page).
   Le volume est de 14 à 18 octets par nœud.
2. **Un message par opération est à proscrire** : 2,7 fois plus lent que le lot pour 2 000 textes. C'est aussi la mesure
   la plus instable : une troisième exécution du cœur actuel a donné 693 ms au lieu de 449 ms.
3. **Une lecture attendue par propriété est à proscrire** : 0,15 ms par aller-retour, soit 250 fois plus lent que les
   mêmes lectures en un appel. Les lectures doivent être explicites et groupées.
4. **Les événements sont interactifs** dès que le transport est corrigé : 0,35 ms du clic à la mutation appliquée.
5. **Le store doit être fusionné côté C++** : chaque message est une tâche distincte du navigateur, React fait donc un
   rendu par message (1 000 pour 1 000 mises à jour). Il faut grouper les mises à jour d'un même cycle, comme l'`Outbox`
   de NiceGUI.
6. **Changer le texte d'un élément détruit l'identité de ses enfants.** Le premier prototype perdait la trace d'un nœud
   texte par ligne : `textContent` remplace les enfants sans que l'interpréteur les oublie. `SET_TEXT` doit viser un nœud
   texte, ou oublier explicitement les enfants qu'il remplace.

### Limites des prototypes relevées en revue

La revue de la PR #385 a montré que l'interpréteur jetable ne tient pas toutes ses promesses ; l'interpréteur embarqué
(#380) doit corriger ces points et les couvrir par des lots hostiles :

- **URL masquées.** Le contrôle par expression régulière laisse passer `"java\tscript:alert(1)"` et
  `"\x01javascript:alert(1)"` : l'analyseur d'URL du navigateur supprime les tabulations et retours à la ligne partout,
  et les caractères de contrôle en tête. Il faut analyser avec `URL` et contrôler le protocole résolu.
- **Validation contre l'état initial seulement.** Un lot qui supprime un nœud puis le réutilise, qui crée un cycle par
  `append`, ou dont la référence d'`insertBefore` n'est pas un enfant du parent, passe la validation puis échoue en cours
  d'application : les étapes déjà appliquées restent et C++ n'est pas prévenu. La validation doit suivre les suppressions
  et la structure produites par le lot ; un échec résiduel doit être signalé comme un rejet.
- **Îlots déplaçables.** `append(x, îlot)` déplace l'élément prêté par la page, contrairement au modèle de propriété.

### Réflexion C++26 vérifiée

GCC 16.1.0 compile le prototype avec `-std=c++26 -freflection` (`__cpp_impl_reflection 202603L`,
`__cpp_expansion_statements 202506L`) : `nonstatic_data_members_of`, `identifier_of`, `enumerators_of`,
`annotations_of_with_type` et `extract`, `define_static_array` et `define_static_string`, `template for`, l'accès aux
membres par `obj.[:m:]`. Points relevés :

- Une annotation ne peut pas contenir un littéral de chaîne (ce n'est pas une valeur structurelle) : `Label` la copie
  par `std::define_static_string`.
- GCC 16.1 refuse un alias `using F = [:type_of(m):]` suivi d'un accès `obj.[:m:]` dans une branche écartée d'un
  `if constexpr` (« consteval-only expressions are only allowed in a constant-evaluated context ») ; lire d'abord le
  membre puis prendre `decltype` contourne le défaut.
- C++26 formate `std::to_string(double)` comme `std::format("{}")` (P2587) : « 1 » et non plus « 1.000000 ».
- Le même fichier compile en C++23, sans réflexion : le prototype signale alors le scénario comme non pris en charge.

| Compilateur | État (2026-10-26) |
| --- | --- |
| GCC | 16.1 (avril 2026) : réflexion, annotations, `template for`, `define_static_*` ; vérifié localement. |
| Clang | Pas de prise en charge publiée ; fork expérimental Bloomberg `clang-p2996` ; intégration amont en cours (source secondaire). |
| MSVC | Aucune prise en charge publique annoncée. |

## Solutions comparées

| | Solution | Forme | Mesure ou coût | React et bibliothèques | Sécurité | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| A | Statu quo, déclarations TypeScript générées | fonctions JS écrites à la main | aucun coût de transport | intact | celle de l'application | **gardée** comme voie principale des interfaces riches |
| B | Flux de mutations regroupé | `Document`, `Node`, lot binaire | +8 à 21 % sur le JS local ; 14–18 octets/nœud | par îlots | liste blanche, lot atomique | **retenue** comme noyau |
| C | Références distantes (proxy) | un message par opération | ×2,7 en écriture, ×250 en lecture | conflit si appliqué hors îlot | idem B | **écartée** ; seules des lectures groupées en restent |
| D | Arbre déclaratif et différence côté C++ | vues immuables, diff | mémoire d'un arbre retenu par navigateur | double propriété si l'arbre englobe React | idem B | **différée**, bibliothèque au-dessus de B |
| E | Signaux C++ liés aux propriétés | `Signal<T>` → mutations ciblées | une mutation par changement | par îlots ou par store | idem B | **différée**, au-dessus de B, avec fusion par cycle |
| F | Intégration par bibliothèque | store externe, `Island`, moteur `react-reconciler` | 0,77 ms clic → rendu React | natif | celle de la bibliothèque | **store et îlots retenus** hors du cœur ; `react-reconciler` écarté |
| G | Fragments HTML (LiveView, htmx) | HTML produit en C++ | analyse HTML à chaque patch | `phx-update="ignore"` | injection HTML à neutraliser | **écartée** : perd l'identité des nœuds et rouvre l'injection |
| H | Hybride : B comme transport, A, D, E, F au-dessus | couches facultatives | celui de B | par îlots et store | celle de B | **décision** |

## Évaluation des coroutines

- **Écritures : pas de `co_await`.** Une mutation ne rend rien ; la rendre attendable cacherait un aller-retour inutile
  ou une fausse synchronisation. Les écritures s'accumulent dans le lot du document et partent ensemble.
- **Lectures : `co_await` explicite et groupé.** Une lecture coûte un aller-retour (0,15 ms au mieux en local, bien plus à
  distance). L'API ne doit offrir que des lectures groupées (`co_await doc.read(a.value(), b.rect())` rendant un tuple),
  sur le modèle de `jsFunctionSender` : arrêt local par jeton, réponse tardive ignorée, le JavaScript déjà lancé continue.
- **Événements : callbacks d'abord, attente ensuite.** Un callback suffit au cas courant. Un événement attendable
  (`co_await button.next("click")`) sert les dialogues séquentiels et les assistants, où la coroutine est la machine à
  états ; il s'annule par le `TaskHandle` et se termine arrêté à la déconnexion. Il n'a pas été prototypé et n'est pas une
  condition d'acceptation du noyau.
- **Décisions synchrones déclarées à l'avance.** `preventDefault` et `stopPropagation` ne peuvent pas attendre C++ : ils
  sont déclarés à l'abonnement (prototype : `submit` annulé).
- **Un lot est une transaction.** Il est validé en entier avant d'être appliqué ; les noms qu'il définit restent acquis
  même s'il est rejeté, car C++ les considère envoyés.
- **Ordre et fil d'exécution.** L'ordre est celui du WebSocket. Un document appartient au fil des callbacks du bridge ;
  une tâche qui le modifie doit reprendre sur ce même contexte. Le point d'envoi automatique (fin de callback, fin de
  reprise d'une tâche, ou cycle cadencé) reste à arrêter avec l'API.

## Réflexion statique C++26

La réflexion apporte la **génération**, pas l'accès au DOM lui-même :

- formulaires, inspecteurs et grilles de propriétés depuis un agrégat (membres, annotations `Label`/`Range`, énumérateurs) ;
- déclarations TypeScript des types échangés, qui ferment la boucle de types avec `wf check`
  ([ADR-014](ADR-014-construction-javascript-dans-le-browser.md)) ;
- sérialisation JSON ou binaire des agrégats, le jour où le bridge transportera des structures.

Elle reste une **couche facultative** : le cœur reste en C++23 ([ADR-001](ADR-001-coeur-cpp23-header-only.md)). La couche
réfléchie produit des **descripteurs** (nom, libellé, type, bornes, accesseurs) qu'une application C++23 peut écrire à la
main ; le générateur de formulaire ne lit que ces descripteurs. Seul GCC 16 la compile aujourd'hui.

## Interaction avec React et les autres bibliothèques

- **Un seul propriétaire par nœud.** C++ n'écrit que dans des îlots que la page lui prête explicitement ; il n'a ni
  sélecteur global ni accès au `document`. Une bibliothèque rend un îlot sans enfants et ne le réconcilie donc pas.
- **Données plutôt que DOM.** Quand l'interface est écrite en React, C++ fournit des valeurs par un store externe
  (`useSyncExternalStore`) et React garde son rendu ; les mises à jour sont fusionnées côté C++.
- **Pas de moteur `react-reconciler` dans le cœur** : son API n'est pas stable et rendrait React obligatoire.
- **Adaptateurs hors du cœur.** Le store et `Island` pour React, et leurs équivalents pour Solid, Svelte, Vue ou Lit, sont
  de petits paquets que `wf` installe ([ADR-010](ADR-010-composants-et-outils-optionnels.md)) ; les éléments personnalisés
  sont une frontière neutre possible.
- **StrictMode et effets.** L'enregistrement d'un îlot doit tolérer le double montage (prototype : même élément
  ré-enregistré, sous-arbre conservé) ; un îlot démonté doit invalider les nœuds C++ qui s'y trouvent.

## Décision

**Solution H** : un noyau de **flux de mutations** (B) sert de transport ; la voie TypeScript (A) reste la voie normale
des interfaces riches ; l'arbre déclaratif (D), les signaux (E), les adaptateurs de bibliothèques (F) et la génération
par réflexion sont des couches facultatives au-dessus du noyau.

### Le noyau

- `webfront::dom::Document` par navigateur connecté, et des `Node` qui sont des identifiants. Les noms sont internés une
  fois par document ; les écritures s'accumulent dans un lot envoyé en une fois.
- Le seul point d'ancrage est un **îlot** prêté par la page. Pour une application entièrement en C++, la page de repli
  embarquée prête un îlot racine.
- Les événements remontent comme appels de callback ; les décisions synchrones (`preventDefault`, `stopPropagation`) sont
  déclarées à l'abonnement.
- Les lectures sont des senders explicites et groupés ; aucune lecture implicite ni synchrone.
- L'interpréteur valide chaque lot en entier, n'utilise jamais de HTML et applique une liste blanche que l'application
  peut étendre, sans jamais autoriser `script`, les attributs `on*` ni les URL `javascript:`. Un rejet est signalé à C++.

### Le protocole

Le format du prototype devient une commande propre de l'[ADR-003](ADR-003-protocole-binaire-et-bornes.md) (par exemple
`domBatch`), au lieu d'un `Uint8Array` passé à une fonction JavaScript, avec des opérations numérotées, des varints, des
noms internés et une **taille maximale de lot** vérifiée des deux côtés. Les événements et les lectures peuvent rester des
appels ordinaires ou recevoir leurs propres commandes, selon les mesures du premier jalon. L'interpréteur est un module
embarqué distinct de `WebFront.js`, que seules les pages qui l'utilisent chargent, afin que `WebFront.js` reste minimal
pour les micro-contrôleurs ; son ajout amende la liste fermée de l'[ADR-016](ADR-016-bootstrap-comme-socle.md).

### Le prérequis de transport

`TCP_NODELAY` est activé sur les connexions acceptées, avec un test de régression, avant toute API DOM : sans lui, chaque
interaction subit 40 ms.

## Critères d'acceptation

Chacun est rattaché à une issue dérivée :

1. (#379) `TCP_NODELAY` est activé et un test montre qu'un callback qui appelle `ui.jsFunction` avant de rendre ne subit plus de
   délai.
2. (#380) La commande de lot est spécifiée dans l'ADR-003, avec des tests d'encodage et de décodage C++, l'interpréteur embarqué
   et une suite Jasmine dans un vrai navigateur sous CEF, WebKitGTK, WebView2 et WKWebView, lots hostiles compris.
3. (#381) L'API `webfront::dom` (document, nœuds, îlots, événements, lectures groupées) a des tests de cycle de vie :
   déconnexion, rechargement de page, plusieurs navigateurs, arrêt ([ADR-006](ADR-006-taches-structurees-et-arret.md)).
4. (#382) Une variante du Game of Life montre un panneau construit en C++ dans l'interface React, avec le store fusionné.
5. (#383) La couche de réflexion est facultative, avec un repli C++23 par descripteurs, et compilée en CI par GCC 16.
6. (#380) Les mesures de cette ADR sont reprises sous CEF et WebView2, et avec un client distant, avec l'interpréteur
   embarqué.

## Options écartées et motifs

- **Une API DOM complète en proxy** (PyScript, Remote DOM sans regroupement) : 2,7 fois plus lente en écriture et
  250 fois en lecture ; elle inciterait à des allers-retours cachés.
- **Des lectures synchrones** : contraires à l'[ADR-004](ADR-004-appels-asynchrones-et-erreurs.md) ; aucun système
  distant étudié ne les offre.
- **Des fragments HTML** : analyse à chaque patch, perte de l'identité des nœuds et de leurs écouteurs, réouverture de
  l'injection.
- **Un moteur `react-reconciler`** : API instable, React rendu obligatoire.
- **Un arbre virtuel C++ dans le cœur** : mémoire retenue par navigateur, inadaptée aux micro-contrôleurs, et doublon des
  bibliothèques ; il reste possible comme couche facultative.
- **Le rendu sur canvas** : accessibilité à reconstruire, contraire au choix du DOM réel.
- **Exiger C++26** : seul GCC 16 compile la réflexion aujourd'hui.

## Conséquences

Positives :

- Une application C++ construit et pilote une interface sans glue JavaScript, à un coût proche du JavaScript local.
- La réflexion rend possibles des IHM par construction, sans imposer C++26.
- React et les autres bibliothèques gardent leur DOM ; l'intégration passe par des îlots et un store.
- La correction de `TCP_NODELAY` profite à toutes les applications existantes.

Négatives acceptées :

- Un second modèle d'interface coexiste avec la voie TypeScript ; la documentation doit dire quand choisir l'un ou l'autre.
- Le cœur gagne un interpréteur embarqué (environ 4 Ko compressés) et une commande de protocole.
- La liste blanche limite ce que C++ peut construire ; l'élargir est une décision de l'application.
- L'identité des nœuds impose des règles (texte, suppression, îlots démontés) que l'API doit rendre difficiles à violer.

## Prescriptions d'implémentation et de génération de code

- Ne jamais présenter une mutation comme synchrone ni attendable ; ne jamais offrir de lecture implicite ou par propriété.
- Ne jamais construire de nœud à partir de HTML, ni autoriser `script`, `on*`, `javascript:` ou `data:` dans `href`/`src`.
- Valider un lot entier avant de l'appliquer, **contre l'état que ses propres opérations produisent** ; signaler tout rejet
  à C++. Un échec pendant l'application reste possible (exception du DOM) : il est signalé comme un rejet.
- Analyser les URL avec l'analyseur du navigateur (`new URL(valeur, document.baseURI)`) et contrôler le protocole résolu,
  jamais par une expression régulière.
- Ne jamais déplacer ni supprimer l'élément d'un îlot : C++ n'en possède que les enfants.
- N'écrire que dans des îlots prêtés ; ne jamais réconcilier un sous-arbre d'une bibliothèque.
- Grouper les mises à jour d'un même cycle (lot, store) ; ne pas envoyer un message par opération.
- Garder le cœur en C++23 ; isoler le code réfléchi derrière `__cpp_impl_reflection`.
- Toute évolution du format suit l'ADR-003 : tests d'encodage et de décodage, test dans un vrai navigateur.

## Issues dérivées

1. #379 : activer `TCP_NODELAY` sur les connexions acceptées, avec un test de régression du délai.
2. #380 : spécifier et implémenter la commande de lot DOM (ADR-003) et l'interpréteur embarqué (amendement de l'ADR-016).
3. #381 : API `webfront::dom` (document, nœuds, îlots, événements, lectures groupées, cycle de vie).
4. #382 : adaptateur React (store fusionné, `Island`) comme paquet `wf`, démontré dans le Game of Life.
5. #383 : descripteurs de types et générateur de formulaires, avec une couche C++26 facultative.
6. #384 : événements attendables (`co_await node.next(event)`) après le noyau.

## Sources et vérification

Consultées le 2026-10-26 :

- React : [README de `react-reconciler`](https://github.com/facebook/react/blob/main/packages/react-reconciler/README.md),
  [`useSyncExternalStore`](https://react.dev/reference/react/useSyncExternalStore).
- [TC39 Signals](https://github.com/tc39/proposal-signals) ; [Shopify Remote DOM](https://github.com/Shopify/remote-dom) ;
  Phoenix LiveView `assets/js/phoenix_live_view/dom_patch.ts` (dépôt `phoenixframework/phoenix_live_view`).
- [Leptos, composants](https://book.leptos.dev/view/01_basic_component.html) ;
  [Dioxus, `Mutation`](https://docs.rs/dioxus-core/latest/dioxus_core/enum.Mutation.html).
- [PyScript, DOM](https://docs.pyscript.net/2026.7.3/user-guide/dom/) ;
  [Reflex, fonctionnement](https://reflex.dev/docs/advanced-onboarding/how-reflex-works) ;
  NiceGUI `nicegui/outbox.py` (dépôt `zauberzeug/nicegui`).
- Réflexion : [P2996R13](https://isocpp.org/files/papers/P2996R13.html),
  [prise en charge C++26 par compilateur](https://en.cppreference.com/cpp/compiler_support/26),
  [fork Bloomberg `clang-p2996`](https://github.com/bloomberg/clang-p2996) ; vérification locale avec GCC 16.1.0.
- Mesures : `test/web/dom/` du commit `ef2bb25` (PR #385), rapports JSON produits par `webkitgtk_dom_prototype`.
