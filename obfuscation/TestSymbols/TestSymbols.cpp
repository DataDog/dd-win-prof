// TestSymbols.cpp - Comprehensive test project for symbol extraction
// This project imports symbols from TestSymbolsDll and adds additional test symbols

#include <iostream>
#include <string>
#include <vector>
#include "../TestSymbolsDll/TestSymbolsDll.h"  // Import DLL exports

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
int g_globalInteger = 42;
double g_globalDouble = 3.14159;
const char* g_globalString = "Global String";
static int g_staticGlobal = 100;

// ============================================================================
// SIMPLE STRUCTURES
// ============================================================================
struct Point
{
    int x;
    int y;
};

struct Rectangle
{
    Point topLeft;
    Point bottomRight;

    int GetWidth() const { return bottomRight.x - topLeft.x; }
    int GetHeight() const { return bottomRight.y - topLeft.y; }
};

// ============================================================================
// CLASSES WITH VARIOUS MEMBERS
// ============================================================================
class SimpleClass
{
public:
    SimpleClass() : m_value(0) {}
    SimpleClass(int value) : m_value(value) {}

    int GetValue() const { return m_value; }
    void SetValue(int value) { m_value = value; }

    static int GetInstanceCount() { return s_instanceCount; }

private:
    int m_value;
    static int s_instanceCount;
};

int SimpleClass::s_instanceCount = 0;

// ============================================================================
// INHERITANCE AND VIRTUAL FUNCTIONS
// Note: Shape and Circle are imported from TestSymbolsDll
// Square is local to this EXE - demonstrates extending DLL classes
// ============================================================================
class Square : public Shape
{
public:
    Square(double side) : m_side(side) {}

    double GetArea() const override { return m_side * m_side; }
    double GetPerimeter() const override { return 4 * m_side; }
    void Draw() const override { std::cout << "Drawing square\n"; }

    double GetSide() const { return m_side; }

private:
    double m_side;
};

// Local helper - not in DLL
void PrintShapeInfo(const Shape* shape)
{
    std::cout << "Shape Name: " << shape->GetName() << std::endl;
    std::cout << "Area: " << shape->GetArea() << std::endl;
    std::cout << "Perimeter: " << shape->GetPerimeter() << std::endl;
}

// ============================================================================
// INTERFACE (Abstract class)
// ============================================================================
class ILogger
{
public:
    virtual ~ILogger() {}
    virtual void Log(const std::string& message) = 0;
    virtual void LogError(const std::string& error) = 0;
    virtual void LogWarning(const std::string& warning) = 0;
};

class ConsoleLogger : public ILogger
{
public:
    void Log(const std::string& message) override
    {
        std::cout << "[INFO] " << message << std::endl;
    }

    void LogError(const std::string& error) override
    {
        std::cerr << "[ERROR] " << error << std::endl;
    }

    void LogWarning(const std::string& warning) override
    {
        std::cout << "[WARNING] " << warning << std::endl;
    }
};

// ============================================================================
// FUNCTION OVERLOADS
// Note: int Add(int,int), double Add(double,double), and int Add(int,int,int)
// are imported from TestSymbolsDll
// String overload is local to this EXE
// ============================================================================
std::string Add(const std::string& a, const std::string& b)
{
    return a + b;
}

// ============================================================================
// TEMPLATE FUNCTIONS
// Note: Max<int> and Max<double> are imported from TestSymbolsDll
// Min is local to this EXE
// ============================================================================
template<typename T>
T Min(T a, T b)
{
    return (a < b) ? a : b;
}

// ============================================================================
// TEMPLATE CLASSES
// ============================================================================
template<typename T>
class Container
{
public:
    Container() : m_data() {}

    void Add(const T& item) { m_data.push_back(item); }
    T Get(size_t index) const { return m_data[index]; }
    size_t Count() const { return m_data.size(); }

private:
    std::vector<T> m_data;
};

// ============================================================================
// NAMESPACES WITH SAME FUNCTION NAMES
// ============================================================================
namespace Math
{
    int Calculate(int x) { return x * 2; }
    double Calculate(double x) { return x * 2.0; }

    namespace Advanced
    {
        int Calculate(int x) { return x * x; }
        double Calculate(double x) { return x * x; }
    }
}

namespace Physics
{
    int Calculate(int x) { return x * 3; }
    double Calculate(double x) { return x * 3.0; }
}

// ============================================================================
// CLASSES WITH SAME NAMES IN DIFFERENT NAMESPACES
// ============================================================================
namespace Graphics
{
    class Renderer
    {
    public:
        void Render() { std::cout << "Graphics::Renderer::Render()\n"; }
        void Clear() { std::cout << "Clearing graphics\n"; }
    };
}

namespace Audio
{
    class Renderer
    {
    public:
        void Render() { std::cout << "Audio::Renderer::Render()\n"; }
        void Play() { std::cout << "Playing audio\n"; }
    };
}

// ============================================================================
// ENUMS
// ============================================================================
enum Color
{
    Red,
    Green,
    Blue,
    Yellow
};

enum class FileMode
{
    Read,
    Write,
    Append,
    ReadWrite
};

// ============================================================================
// UNIONS
// ============================================================================
union Data
{
    int intValue;
    float floatValue;
    char charValue;
};

// ============================================================================
// NESTED CLASSES
// ============================================================================
class OuterClass
{
public:
    class InnerClass
    {
    public:
        void InnerMethod() { std::cout << "Inner method\n"; }

        class DeepInnerClass
        {
        public:
            void DeepMethod() { std::cout << "Deep inner method\n"; }
        };
    };

    void OuterMethod() { std::cout << "Outer method\n"; }
};

// ============================================================================
// OPERATOR OVERLOADING
// Note: Complex class with operator+, operator*, operator== is imported from TestSymbolsDll
// ============================================================================

// Local operator overload example for a different type
class Vector2D
{
public:
    Vector2D(double x = 0, double y = 0) : m_x(x), m_y(y) {}

    Vector2D operator+(const Vector2D& other) const
    {
        return Vector2D(m_x + other.m_x, m_y + other.m_y);
    }

    Vector2D operator-(const Vector2D& other) const
    {
        return Vector2D(m_x - other.m_x, m_y - other.m_y);
    }

    double GetX() const { return m_x; }
    double GetY() const { return m_y; }

private:
    double m_x;
    double m_y;
};

// ============================================================================
// FUNCTION POINTERS AND CALLBACKS
// ============================================================================
typedef void (*CallbackFunction)(int);

void InvokeCallback(CallbackFunction callback, int value)
{
    if (callback)
        callback(value);
}

void MyCallback(int value)
{
    std::cout << "Callback called with value: " << value << std::endl;
}

// ============================================================================
// STATIC FUNCTIONS
// ============================================================================
static void StaticHelperFunction()
{
    std::cout << "Static helper function\n";
}

static int StaticCalculation(int x, int y)
{
    return x * y + x + y;
}

// ============================================================================
// MAIN FUNCTION - Test all symbols
// ============================================================================
int main()
{
    std::cout << "=== TestSymbols - Comprehensive Symbol Test ===" << std::endl;

    // Test global variables
    std::cout << "\nGlobal variables:" << std::endl;
    std::cout << "g_globalInteger: " << g_globalInteger << std::endl;
    std::cout << "g_globalDouble: " << g_globalDouble << std::endl;

    // Test structures
    Point p1 = {10, 20};
    Rectangle rect = {{0, 0}, {100, 50}};
    std::cout << "\nRectangle: " << rect.GetWidth() << "x" << rect.GetHeight() << std::endl;

    // Test simple class
    SimpleClass obj(42);
    std::cout << "\nSimpleClass value: " << obj.GetValue() << std::endl;

    // Test inheritance and polymorphism (using DLL's Circle and local Square)
    Shape* shapes[2];
    shapes[0] = new Circle(5.0);  // From DLL
    shapes[1] = new Square(4.0);  // Local to EXE

    std::cout << "\nShapes (DLL Circle + Local Square):" << std::endl;
    for (int i = 0; i < 2; i++)
    {
        shapes[i]->SetName("Test Shape");
        PrintShapeInfo(shapes[i]);
        shapes[i]->Draw();
    }

    delete shapes[0];
    delete shapes[1];

    // Test interface
    ILogger* logger = new ConsoleLogger();
    logger->Log("Test message");
    logger->LogWarning("Test warning");
    delete logger;

    // Test function overloads (from DLL)
    std::cout << "\nFunction overloads (DLL):" << std::endl;
    std::cout << "Add(1, 2): " << Add(1, 2) << std::endl;  // DLL: int Add(int, int)
    std::cout << "Add(1.5, 2.5): " << Add(1.5, 2.5) << std::endl;  // DLL: double Add(double, double)
    std::cout << "Add(1, 2, 3): " << Add(1, 2, 3) << std::endl;  // DLL: int Add(int, int, int)
    std::cout << "Add(\"Hello\", \"World\"): " << Add(std::string("Hello"), std::string("World")) << std::endl;  // Local: string Add

    // Test templates (from DLL)
    std::cout << "\nTemplates (DLL):" << std::endl;
    std::cout << "Max<int>(10, 20): " << Max<int>(10, 20) << std::endl;  // DLL explicit instantiation
    std::cout << "Max<double>(3.14, 2.71): " << Max<double>(3.14, 2.71) << std::endl;  // DLL explicit instantiation
    std::cout << "Min<int>(10, 20): " << Min(10, 20) << std::endl;  // Local template

    Container<int> intContainer;
    intContainer.Add(100);
    intContainer.Add(200);
    std::cout << "Container count: " << intContainer.Count() << std::endl;

    // Test namespaces
    std::cout << "\nNamespaces:" << std::endl;
    std::cout << "Math::Calculate(5): " << Math::Calculate(5) << std::endl;
    std::cout << "Math::Advanced::Calculate(5): " << Math::Advanced::Calculate(5) << std::endl;
    std::cout << "Physics::Calculate(5): " << Physics::Calculate(5) << std::endl;

    // Test same class names in different namespaces
    Graphics::Renderer graphicsRenderer;
    Audio::Renderer audioRenderer;
    graphicsRenderer.Render();
    audioRenderer.Render();

    // Test enums
    Color color = Red;
    FileMode mode = FileMode::ReadWrite;

    // Test nested classes
    OuterClass outer;
    outer.OuterMethod();
    OuterClass::InnerClass inner;
    inner.InnerMethod();

    // Test operator overloading (using DLL's Complex class)
    std::cout << "\nOperator Overloading (DLL Complex):" << std::endl;
    Complex c1(3, 4);
    Complex c2(1, 2);
    Complex c3 = c1 + c2;
    Complex c4 = c1 * c2;
    std::cout << "Complex c1(3,4) + c2(1,2) = (" << c3.GetReal() << ", " << c3.GetImag() << ")" << std::endl;
    std::cout << "Complex c1 * c2 = (" << c4.GetReal() << ", " << c4.GetImag() << ")" << std::endl;
    std::cout << "Complex c1 == c2: " << (c1 == c2 ? "true" : "false") << std::endl;

    // Test local Vector2D operator overloading
    std::cout << "\nOperator Overloading (Local Vector2D):" << std::endl;
    Vector2D v1(3, 4);
    Vector2D v2(1, 2);
    Vector2D v3 = v1 + v2;
    std::cout << "Vector2D v1(3,4) + v2(1,2) = (" << v3.GetX() << ", " << v3.GetY() << ")" << std::endl;

    // Test callbacks
    InvokeCallback(MyCallback, 42);

    // Test static functions
    StaticHelperFunction();
    std::cout << "StaticCalculation(3, 4): " << StaticCalculation(3, 4) << std::endl;

    std::cout << "\n=== All tests completed ===" << std::endl;

    return 0;
}

