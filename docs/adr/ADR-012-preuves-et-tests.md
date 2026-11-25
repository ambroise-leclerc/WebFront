# ADR-012 : Éprouver les contrats par des tests déterministes et un navigateur réel

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2020-04-19 | Acceptée | Ambroise Leclerc | Décision initiale : structure de tests unitaires Catch2. | [3acfe58](https://github.com/ambroise-leclerc/WebFront/commit/3acfe58b97dce207063c247ebc00906a4bbe749a) |
| 1.1 | 2023-05-12 | Acceptée | Ambroise Leclerc | Première application de tests Jasmine dans un navigateur. | [58e69f5](https://github.com/ambroise-leclerc/WebFront/commit/58e69f50c48f183d766886c9d38269337e6afbae) |
| 1.2 | 2026-07-16 | Acceptée | Ambroise Leclerc | Lanceur Jasmine autonome : compte rendu, fermeture et résultat de processus exploitable. | [d77668f](https://github.com/ambroise-leclerc/WebFront/commit/d77668fe64d17d0fa459008c95b56f4c8cace930) |
| 1.3 | 2026-10-13 | Acceptée | Ambroise Leclerc | Vérification des exemples documentés et du consommateur add_subdirectory. | [674d554](https://github.com/ambroise-leclerc/WebFront/commit/674d554ef060ae824a59762c4da3f425aebf9742) |
| 1.4 | 2026-10-18 | Acceptée | Ambroise Leclerc | Scénarios de cycle de vie mutualisés entre frontends. | [a3ba113](https://github.com/ambroise-leclerc/WebFront/commit/a3ba1134b1f3ad03dc01f24fd59e2b47f47c3466) |
| 1.5 | 2026-10-18 | Acceptée | Ambroise Leclerc | Durcissement des décodeurs, assainisseurs et tests par génération d’entrées dans la CI. | [d6f086d](https://github.com/ambroise-leclerc/WebFront/commit/d6f086d50350379162ab01e7bdba2a734646d24d) |
| 1.6 | 2026-10-19 | Acceptée | Ambroise Leclerc | Contrôles hors ligne de fraîcheur et de contenu des ressources gzip reproductibles. | [b031ab5](https://github.com/ambroise-leclerc/WebFront/commit/b031ab58ea9cc32290d701094c1333bab455e67f) |
| 1.7 | 2026-11-14 | Acceptée | Ambroise Leclerc | Catch2 remplacé par `test/unit/Bdd.hpp`, un cadre BDD en un seul en-tête. | [54c0d0f](https://github.com/ambroise-leclerc/WebFront/commit/54c0d0fd35a414e01e2cff0c27882695ca9a4e9d) |
| 1.8 | 2026-11-23 | Acceptée | Ambroise Leclerc | Le cadre BDD devient une interface publique maintenue : `WebFront::Bdd`, `webfront_bdd_discover_tests` et `docs/BDD.md`. | [1386947](https://github.com/ambroise-leclerc/WebFront/commit/138694770e3849cd089f7a71120a332ed445f496) |
| 1.9 | 2026-11-25 | Acceptée | Ambroise Leclerc | Le cadre BDD se découpe en un cœur léger et des parties optionnelles ; `checkEqual` rapporte ses opérandes, `approx` compare des flottants. | [4c7a768](https://github.com/ambroise-leclerc/WebFront/commit/4c7a7680feee55bdbd8f0cec54e76f1e6ed06b7f) |

## État et chronologie

**Acceptée.** Décision initiale : **2020-04-19** (version 1.0).
Dernière révision : **2026-11-25** (version 1.9).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

La justesse isolée du codec C++ ne prouve ni la compatibilité du JavaScript embarqué ni la
fermeture de la fenêtre avec un statut exploitable. Les commandes publiées et l’intégration
comme sous-projet constituent elles aussi des contrats observables.

## Décision

Que les tests unitaires, sous `test/unit/`, vérifient unités, états et erreurs à l’aide de simulacres déterministes.
Ils emploient `include/webfront/testing/Bdd.hpp`, un cadre BDD en en-têtes seuls, interface publique maintenue (`docs/BDD.md`) ;
`Bdd.hpp` réunit un cœur léger, `bdd/Core.hpp`, que chaque fichier de test peut inclure seul, et des parties optionnelles
(`Matchers`, `Approx`, `Eventually`, `Runner`) ; le cœur ne tire ni `<chrono>`, ni `<thread>`, ni `<fstream>`. Un
scénario est un objet `const Scenario` nommé, ses sections `given`/`when`/`then` sont des lambdas, et `require`/`check`
situent un échec par `std::source_location`. Le cadre rejoue un scénario jusqu’à avoir parcouru chacune de ses
feuilles, une nouvelle par passe, comme le faisait Catch2, dont seul ce sous-ensemble servait et dont la
compilation pesait sur chaque construction des tests.
Jasmine sous `test/web/` exécute les modules dans un vrai moteur, attend le reporting vers
C++, se ferme et fournit un statut de processus significatif. CTest borne les tests par des
timeouts. La CI Linux utilise CEF/Xvfb ; les moteurs natifs ont leurs suites de cycle de vie
lorsque configurés.

Le socle de validation est une configuration Release sans CEF avec `WEBFRONT_ENABLE_TESTING=ON`,
construction puis CTest. Les changements de bridge, cycle de vie navigateur, modules servis,
assets Jasmine ou frontend CEF exigent aussi l’intégration CEF/Xvfb. Les voies natives
complètent cette couverture selon la modification.

Les assainisseurs et les tests par génération d’entrées éprouvent les frontières d’analyse.
Les contrôles de fraîcheur protègent les ressources ; `NewcomerDocsMatchExamples` vérifie le
parcours documenté et
`AddSubdirectoryConsumer` l’intégration CMake. Les tests n’ont besoin ni d’un navigateur
préexistant, ni d’une intervention humaine, ni du réseau public après configuration.

## Options écartées et motifs

- Se contenter de mocks C++ : ne détecte pas la dérive du codec JS ou des assets servis.
- Tests manuels comme seule preuve : ne garantissent ni résultat CI ni fermeture autonome.
- Accéder au registre npm réel dans les tests : remplacer par des fixtures locales afin de
  conserver reproductibilité et fonctionnement hors ligne.

## Conséquences

- **Bénéfice :** erreurs de contrats observables au niveau où elles se produisent et parcours
  de démarrage vérifié automatiquement.
- **Coût accepté :** dépendances de test, moteurs et jobs spécifiques supplémentaires.
- **Risque :** succès apparent d’un runner qui n’a pas exécuté ses specs ; exiger reporting,
  statut non nul sur échec et timeout, sans sleeps arbitraires pour synchroniser les scénarios.

## Prescriptions d’implémentation et de génération de code

- Pour chaque correction, qu’une régression échoue d’abord au niveau pertinent le plus bas.
- Écrire les scénarios avec `Scenario`, `given`/`when`/`then` et `require`/`check` de `Bdd.hpp` ;
  traiter les Promises dans Jasmine.
- Comparer deux valeurs avec `requireEqual`/`checkEqual`, dont l’échec rapporte les deux opérandes, plutôt qu’avec
  `require(a == b)` ; un flottant mesuré avec `approx`, jamais par une égalité exacte.
- Attendre un état asynchrone avec `requireEventually`/`checkEventually` et `requireReady`/`checkReady` de `Bdd.hpp`
  (30 s par défaut, multiplié par `WEBFRONT_TEST_TIMEOUT_FACTOR`), jamais par une pause fixe ni par un helper local ;
  l’échec indique la ligne de l’attente, le délai et le temps attendu. `waitUntil`/`waitReady` n’enregistrent rien :
  ils servent à un thread qui ne peut pas affirmer, ou avant un nettoyage, et le scénario affirme ensuite le résultat.
- Que toute modification du protocole couvre encodeur et décodeur dans les deux langages,
  ainsi que le trajet réel dans le navigateur.
- Construire hors source, demander les options explicitement et préserver les builds existants.
- Documenter les vérifications réellement exécutées et leurs limites ; une preuve Linux ne
  vaut pas qualification de tous les moteurs et plateformes.

## Sources et vérification

- [Guide contributeur](../../AGENTS.md), [CI](../../.github/workflows/BuildAndTest.yml),
  [CMake des tests](../../test/unit/CMakeLists.txt), [runner navigateur](../../test/web/CMakeLists.txt).
- Commandes de référence : [README](../../README.md) et [frontends](../FRONTENDS.md).
- Toute nouvelle architecture doit préciser ses preuves attendues, ses échecs observables et
  les limites encore ouvertes dans [PROJECT_STATUS](../PROJECT_STATUS.md).
