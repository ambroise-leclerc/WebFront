# ADR-008 : Limiter les origines du bridge sans promettre un sandbox

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-18 | Acceptée | Ambroise Leclerc | Création initiale : politique commune des origines du bridge, de la navigation et des popups. | [d59db71](https://github.com/ambroise-leclerc/WebFront/commit/d59db712f1d390abe1457a5046b904bd330b85cc) |

## Statut et date

**Acceptée.** Création initiale : **2026-10-18** (version 1.0).
Dernière évolution : **2026-10-18** (version 1.0).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Une page locale ayant accès au bridge peut appeler les fonctions C++ enregistrées. Une
fenêtre embarquée ou un outil installé n’isole pas automatiquement un script hostile.
La politique commune doit protéger des connexions navigateur étrangères sans être présentée
comme une authentification des clients natifs.

## Décision

`WebFrontConfig::contentPolicy` fournit une politique par application, copiée au serveur
et transmise aux frontends intégrés. Par défaut, le bridge autorise l’origine applicative,
la navigation principale embarquée reste sur cette origine et les popups sont bloqués.
Des listes d’origines supplémentaires règlent séparément bridge et navigation.

Vérifier l’Origin avant la réponse HTTP 101 et avant création du `WebLink`. Les origines
malformées, opaques, dupliquées ou interdites produisent 403. Les clients sans Origin sont
acceptés par défaut (`allowMissingOrigin`), ce qui ne prouve aucune identité : un client natif
peut forger cet en-tête. `Host` ne donne pas une permission de bridge.

Les dérogations de développement sont explicites. DevTools et menus restent activés par
défaut. Le navigateur système contrôle sa propre navigation ; le serveur y applique quand même
le contrôle du bridge. Les capacités des moteurs diffèrent, notamment les popups WKWebView.
WebFront cible du contenu de confiance, sans TLS ni authentification intégrés.

## Alternatives et arbitrage

- Accepter toute origine par défaut : remplacé par le contrôle d’origine commun.
- Traiter Origin comme un secret ou une identité : incompatible avec les clients natifs.
- Présenter la navigation principale comme un filtre réseau complet : les sous-ressources,
  iframes et scripts de même origine ne sont pas isolés par cette politique.

## Conséquences

- **Bénéfice :** refus cohérent des pages navigateur d’autres origines avant établissement du pont.
- **Coût accepté :** configuration requise pour une interface de développement sur un autre port.
- **Risque :** confiance excessive dans une allow-list ; un script autorisé partage l’autorité
  de l’application, et les outils installés aussi. Conserver loopback sauf choix explicite.

## Règles d’implémentation et de génération de code

- Valider la configuration avant bind ; conserver les comparaisons d’origines normalisées
  (schéma, hôte, port), sans assimiler `localhost` et `127.0.0.1`.
- Ne pas autoriser le bridge parce que la navigation l’est, ni l’inverse.
- Ne pas contourner les contrôles pour un nouvel outil ou exemple ; demander une configuration
  explicite si l’origine change.
- Tester refus avec contrôles positifs dans le vrai navigateur ; vérifier que `ready` rejette.
- Ne pas annoncer sandbox, isolation des plugins ou accès distant sécurisé.

## Références et vérification

- [Politique et capacités détaillées](../CONTENT_POLICY.md),
  [ContentPolicy](../../include/ContentPolicy.hpp).
- Tests : [ContentPolicyTests](../../test/ContentPolicyTests.cpp),
  [content-policy-specs](../../webtest/js/content-policy-specs.mjs).
- Voir [ADR-007](ADR-007-frontends-explicites.md) et [ADR-010](ADR-010-composants-et-outils-optionnels.md).
