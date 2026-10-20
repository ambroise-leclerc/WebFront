# ADR-012 : Démontrer les contrats par tests déterministes et intégration navigateur

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2020-04-19 | Acceptée | Ambroise Leclerc | Création initiale : structure de tests unitaires Catch2. | [3acfe58](https://github.com/ambroise-leclerc/WebFront/commit/3acfe58b97dce207063c247ebc00906a4bbe749a) |
| 1.1 | 2023-05-12 | Acceptée | Ambroise Leclerc | Première application de tests Jasmine dans un navigateur. | [58e69f5](https://github.com/ambroise-leclerc/WebFront/commit/58e69f50c48f183d766886c9d38269337e6afbae) |
| 1.2 | 2026-07-16 | Acceptée | Ambroise Leclerc | Runner Jasmine autonome : reporting, fermeture et résultat de processus exploitable. | [d77668f](https://github.com/ambroise-leclerc/WebFront/commit/d77668fe64d17d0fa459008c95b56f4c8cace930) |
| 1.3 | 2026-10-13 | Acceptée | Ambroise Leclerc | Vérification des exemples documentés et du consommateur add_subdirectory. | [674d554](https://github.com/ambroise-leclerc/WebFront/commit/674d554ef060ae824a59762c4da3f425aebf9742) |
| 1.4 | 2026-10-18 | Acceptée | Ambroise Leclerc | Scénarios de cycle de vie mutualisés entre frontends. | [a3ba113](https://github.com/ambroise-leclerc/WebFront/commit/a3ba1134b1f3ad03dc01f24fd59e2b47f47c3466) |
| 1.5 | 2026-10-18 | Acceptée | Ambroise Leclerc | Durcissement des décodeurs, sanitizers et fuzzing dans la CI. | [d6f086d](https://github.com/ambroise-leclerc/WebFront/commit/d6f086d50350379162ab01e7bdba2a734646d24d) |
| 1.6 | 2026-10-19 | Acceptée | Ambroise Leclerc | Contrôles hors ligne de fraîcheur et de contenu des assets gzip reproductibles. | [b031ab5](https://github.com/ambroise-leclerc/WebFront/commit/b031ab58ea9cc32290d701094c1333bab455e67f) |

## Statut et date

**Acceptée.** Création initiale : **2020-04-19** (version 1.0).
Dernière évolution : **2026-10-19** (version 1.6).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Un codec C++ correct isolément ne prouve pas que le JavaScript réellement embarqué est
compatible, ni que la fenêtre ferme et renvoie un statut exploitable. Les commandes de la
documentation et l’intégration dans un parent sont également des comportements à préserver.

## Décision

Catch2 sous `test/` vérifie les unités, états et erreurs avec des mocks déterministes.
Jasmine sous `webtest/` exécute les modules dans un vrai moteur, attend le reporting vers
C++, se ferme et fournit un statut de processus significatif. CTest borne les tests par des
timeouts. La CI Linux utilise CEF/Xvfb ; les moteurs natifs ont leurs suites de cycle de vie
lorsque configurés.

Le socle de validation est une configuration Release sans CEF avec `ENABLE_TESTING=ON`,
construction puis CTest. Les changements de bridge, cycle de vie navigateur, modules servis,
assets Jasmine ou frontend CEF exigent aussi l’intégration CEF/Xvfb. Les voies natives
complètent cette couverture selon la modification.

Sanitizers et fuzzing durcissent les frontières de parsing. Les contrôles de fraîcheur
protègent les assets ; `NewcomerDocsMatchExamples` vérifie le parcours documenté et
`AddSubdirectoryConsumer` l’intégration CMake. Les tests n’ont besoin ni d’un navigateur
préexistant, ni d’une intervention humaine, ni du réseau public après configuration.

## Alternatives et arbitrage

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

## Règles d’implémentation et de génération de code

- Pour une correction, écrire une régression qui échoue avant au niveau utile le plus bas.
- Suivre `SCENARIO`/`GIVEN`/`WHEN`/`THEN` dans Catch2 ; traiter les Promises dans Jasmine.
- Pour une modification du protocole, couvrir encodeur et décodeur dans les deux langages et
  le trajet réel dans le navigateur.
- Construire hors source, demander les options explicitement et préserver les builds existants.
- Documenter les vérifications réellement exécutées et leurs limites ; une preuve Linux ne
  vaut pas qualification de tous les moteurs et plateformes.

## Références et vérification

- [Guide contributeur](../../AGENTS.md), [CI](../../.github/workflows/BuildAndTest.yml),
  [CMake des tests](../../test/CMakeLists.txt), [runner navigateur](../../webtest/CMakeLists.txt).
- Commandes de référence : [README](../../README.md) et [frontends](../FRONTENDS.md).
- Toute nouvelle architecture doit préciser ses preuves attendues, ses échecs observables et
  les limites encore ouvertes dans [PROJECT_STATUS](../PROJECT_STATUS.md).
