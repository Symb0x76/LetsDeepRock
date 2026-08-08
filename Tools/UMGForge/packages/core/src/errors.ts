export class RevisionConflictError extends Error {
  constructor(public readonly expected: number, public readonly actual: number) {
    super(`Revision conflict: expected ${expected}, actual ${actual}`);
    this.name = "RevisionConflictError";
  }
}

export class ValidationError extends Error {
  constructor(public readonly issues: string[]) {
    super(`Validation failed:\n${issues.join("\n")}`);
    this.name = "ValidationError";
  }
}
