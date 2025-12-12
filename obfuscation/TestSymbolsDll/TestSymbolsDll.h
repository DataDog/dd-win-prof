// TestSymbolsDll.h - Header for test DLL with mixed PUBLIC and PRIVATE symbols

#pragma once

#ifdef TESTSYMBOLS_DLL_EXPORTS
#define TESTSYMBOLS_API __declspec(dllexport)
#else
#define TESTSYMBOLS_API __declspec(dllimport)
#endif

// ============================================================================
// PUBLIC: Exported classes
// ============================================================================

// PUBLIC: Base shape class with virtual functions
class TESTSYMBOLS_API Shape
{
public:
    virtual ~Shape() {}
    virtual double GetArea() const = 0;
    virtual double GetPerimeter() const = 0;
    virtual void Draw() const;

    void SetName(const char* name);
    const char* GetName() const;

private:
    char m_name[64];
};

// PUBLIC: Circle class
class TESTSYMBOLS_API Circle : public Shape
{
public:
    explicit Circle(double radius);
    ~Circle() override;

    double GetArea() const override;
    double GetPerimeter() const override;
    void Draw() const override;

    double GetRadius() const;

private:
    double m_radius;
};

// PRIVATE: This class is NOT exported (internal only)
class InternalHelper
{
public:
    static int Calculate(int x);
    static double ProcessData(double value);
};

// ============================================================================
// PUBLIC: Exported functions
// ============================================================================

// PUBLIC: Exported function overloads
TESTSYMBOLS_API int Add(int a, int b);
TESTSYMBOLS_API double Add(double a, double b);
TESTSYMBOLS_API int Add(int a, int b, int c);

// PUBLIC: Exported template function
// Note: Templates in headers can't use __declspec(dllexport) directly
// We'll explicitly instantiate them in the .cpp file
template<typename T>
T Max(T a, T b)
{
    return (a > b) ? a : b;
}

// Explicit template declarations (will be instantiated in .cpp)
extern template TESTSYMBOLS_API int Max<int>(int, int);
extern template TESTSYMBOLS_API double Max<double>(double, double);

// PRIVATE: Internal functions (not exported)
int InternalAdd(int a, int b);
void InternalProcess();

// ============================================================================
// PUBLIC: Exported operator overloading
// ============================================================================

class TESTSYMBOLS_API Complex
{
public:
    Complex(double real = 0, double imag = 0);
    ~Complex();

    Complex operator+(const Complex& other) const;
    Complex operator*(const Complex& other) const;
    bool operator==(const Complex& other) const;

    double GetReal() const;
    double GetImag() const;

private:
    double m_real;
    double m_imag;
};

