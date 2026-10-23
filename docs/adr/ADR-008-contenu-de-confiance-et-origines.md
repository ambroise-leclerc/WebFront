# ADR-008 : Restreindre les origines du pont sans promettre d’isolation

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-18 | Acceptée | Ambroise Leclerc | Décision initiale : politique commune des origines du pont, de la navigation et des fenêtres surgissantes. | [d59db71](https://github.com/ambroise-leclerc/WebFront/commit/d59db712f1d390abe1457a5046b904bd330b85cc) |

## État et chronologie

**Acceptée.** Décision initiale : **2026-10-18** (version 1.0).
Dernière révision : **2026-10-18** (version 1.0).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

Toute page locale admise sur le pont peut appeler les fonctions C++ enregistrées. Ni fenêtre
intégrée ni outil installé ne confinent, à eux seuls, un script hostile. La politique commune
doit écarter les connexions de navigateur d’origine étrangère, sans se faire passer pour une
authentification des clients natifs.

## Décision

`WebFrontConfig::contentPolicy` fournit une politique par application, copiée au serveur
et transmise aux frontends intégrés. Par défaut, le bridge autorise l’origine applicative,
la navigation principale embarquée reste sur cette origine et les popups sont bloqués.
Des listes d’origines supplémentaires règlent séparément bridge et navigation.

Que l’en-tête `Origin` soit vérifié avant la réponse HTTP 101 et la création du `WebLink`.
Les origines malformées, opaques, dupliquées ou interdites produisent 403. Les clients sans Origin sont
acceptés par défaut (`allowMissingOrigin`), ce qui ne prouve aucune identité : un client natif
peut forger cet en-tête. `Host` ne donne pas une permission de bridge.

Que les dérogations de développement demeurent explicites. DevTools et menus restent activés
par défaut. Le navigateur système contrôle sa propre navigation ; le serveur y applique néanmoins
le contrôle du bridge. Les capacités des moteurs diffèrent, notamment les popups WKWebView.
WebFront cible du contenu de confiance, sans TLS ni authentification intégrés.

## Options écartées et motifs

- Accepter toute origine par défaut : remplacé par le contrôle d’origine commun.
- Traiter Origin comme un secret ou une identité : incompatible avec les clients natifs.
- Présenter la navigation principale comme un filtre réseau complet : les sous-ressources,
  iframes et scripts de même origine ne sont pas isolés par cette politique.

## Conséquences

- **Bénéfice :** refus des connexions au bridge provenant d’autres origines, avant que le pont
  ne soit établi.
- **Coût accepté :** configuration requise pour une interface de développement sur un autre port.
- **Risque :** confiance excessive dans une allow-list ; un script autorisé partage l’autorité
  de l’application, et les outils installés aussi. Conserver loopback sauf choix explicite.

## Prescriptions d’implémentation et de génération de code

- Que la configuration soit validée avant la mise en écoute ; que les origines soient comparées
  après normalisation du schéma, de l’hôte et du port, sans assimiler `localhost` à `127.0.0.1`.
- Que l’autorisation de naviguer n’emporte pas celle d’utiliser le pont, ni l’inverse.
- Ne pas contourner les contrôles pour un nouvel outil ou exemple ; demander une configuration
  explicite si l’origine change.
- Tester refus avec contrôles positifs dans le vrai navigateur ; vérifier que `ready` rejette.
- Qu’aucune isolation des extensions ni aucun accès distant sécurisé ne soient annoncés.

## Sources et vérification

- [Politique et capacités détaillées](../CONTENT_POLICY.md),
  [ContentPolicy](../../include/webfront/ContentPolicy.hpp).
- Tests : [ContentPolicyTests](../../test/unit/ContentPolicyTests.cpp),
  [content-policy-specs](../../test/web/suite/specs/content-policy-specs.mjs).
- Voir [ADR-007](ADR-007-frontends-explicites.md) et [ADR-010](ADR-010-composants-et-outils-optionnels.md).
